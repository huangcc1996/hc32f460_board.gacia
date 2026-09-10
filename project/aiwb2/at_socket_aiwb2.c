/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-04     opencode     first version (Ai-WB2 Combo-AT)
 */

#include <stdio.h>
#include <string.h>

#include <at_device_aiwb2.h>

#define LOG_TAG "at.skt.aiwb2"
#include <at_log.h>

#if defined(AT_DEVICE_USING_AIWB2) && defined(AT_USING_SOCKET)

#define AIWB2_SEND_WAIT_TIMEOUT 10 * RT_TICK_PER_SECOND

static at_evt_cb_t at_evt_cb_set[] = {
    [AT_SOCKET_EVT_RECV] = NULL,
    [AT_SOCKET_EVT_CLOSED] = NULL,
#ifdef AT_USING_SOCKET_SERVER
    [AT_SOCKET_EVT_CONNECTED] = NULL,
#endif
};

static void urc_recv_func(struct at_client* client, const char* data, rt_size_t size);
static void urc_close_func(struct at_client* client, const char* data, rt_size_t size);
static void urc_reconn_func(struct at_client* client, const char* data, rt_size_t size);
static void urc_autodel_func(struct at_client* client, const char* data, rt_size_t size);
#ifdef AT_USING_SOCKET_SERVER
static void urc_seed_func(struct at_client* client, const char* data, rt_size_t size);
#endif

/**
 * @brief Ai-WB2 socket URC 表
 * @details 事件统一前缀 +EVENT:, 数据在行内(主动模式 SOCKETRECVCFG=1);
 *          SocketDisconnect 为标准拼写单 s(真机确认, 与官方 skill 的
 *          SocketDissconnect 双 s 不符); SocketReconnect/SocketAutoDel 仅日志
 */
static const struct at_urc aiwb2_socket_urc_table[] = {
    {"+EVENT:SocketDown,", "\r\n", urc_recv_func},
    {"+EVENT:SocketDisconnect,", "\r\n", urc_close_func},
    {"+EVENT:SocketReconnect,", "\r\n", urc_reconn_func},
    {"+EVENT:SocketAutoDel,", "\r\n", urc_autodel_func},
#ifdef AT_USING_SOCKET_SERVER
    {"+EVENT:SocketSeed,", "\r\n", urc_seed_func},
#endif
};

/**
 * @brief 按 ConID 遍历定位 at_socket 对象
 * @param device AT 设备对象
 * @param con_id 模组连接 ID(驱动显式指定 = 下标+1)
 * @return 匹配的 at_socket 对象, 未找到返回 RT_NULL
 * @note 采用遍历定位(非 sockets[con_id-1] 直取), 容错未指定/异常场景
 */
static struct at_socket* aiwb2_find_socket(struct at_device* device, int con_id)
{
    int i;

    if (device == RT_NULL || con_id <= 0) {
        return RT_NULL;
    }
    for (i = 0; i < (int)device->class->socket_num; i++) {
        if ((int)(intptr_t)device->sockets[i].user_data == con_id) {
            return &device->sockets[i];
        }
    }
    return RT_NULL;
}

/**
 * @brief 等待发送数据的 OK/ERROR 命令响应行
 * @param device  AT 设备对象
 * @param timeout 等待超时(tick)
 * @return 0 成功 / -5 无内存 / -2 超时 / -1 失败(行内含 "ERROR")
 * @details 数据已由 at_client_obj_send 发出, 模组收满后回 OK/ERROR, 但此时
 *          无等待中的命令(at_obj_exec_cmd 已在 '>' 提示符处返回), 孤儿行会
 *          被解析线程忽略——仿 at_client_obj_wait_connect 自建等待: 直接置
 *          client->resp 让解析线程匹配 OK/ERROR 行。ConID 不存在时模组静默
 *          丢弃数据无响应(真机确认), 本函数超时返回 -2, 由上层按发送失败处理。
 */
static int aiwb2_send_wait(struct at_device* device, rt_tick_t timeout)
{
    struct at_client* client = device->client;
    at_response_t resp = RT_NULL;
    int result = RT_EOK;

    resp = at_create_resp(64, 0, timeout);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    rt_mutex_take(client->lock, RT_WAITING_FOREVER);

    client->resp_status = AT_RESP_OK;
    resp->buf_len = 0;
    resp->line_counts = 0;
    client->resp = resp;
    rt_sem_control(client->resp_notice, RT_IPC_CMD_RESET, RT_NULL);

    if (rt_sem_take(client->resp_notice, timeout) != RT_EOK) {
        client->resp_status = AT_RESP_TIMEOUT;
        result = -RT_ETIMEOUT;
    }
    else if (client->resp_status != AT_RESP_OK) {
        result = -RT_ERROR; /* 行内含 "ERROR"(如 +SOCKETSEND:101 后跟 ERROR) */
    }

    client->resp = RT_NULL;
    rt_mutex_release(client->lock);
    at_delete_resp(resp);

    return result;
}

/**
 * @brief 关闭 socket 连接, AT+SOCKETDEL=<ConID>
 * @param socket 当前 at_socket 对象
 * @return 0 成功 / -1 失败(发送命令或响应错误) / -5 无内存
 */
static int aiwb2_socket_close(struct at_socket* socket)
{
    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    int con_id = (int)(intptr_t)socket->user_data;
    struct at_device* device = (struct at_device*)socket->device;

    resp = at_create_resp(64, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    result = at_obj_exec_cmd(device->client, resp, "AT+SOCKETDEL=%d", con_id);

    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * @brief 创建 TCP/UDP client 连接, AT+SOCKET=<type>,<host>,<port>,0,<conID>
 * @param socket    当前 at_socket 对象
 * @param ip        服务器 IP 或域名(域名直写, 真机确认可行)
 * @param port      服务器端口
 * @param type      socket 类型(AT_SOCKET_TCP / AT_SOCKET_UDP)
 * @param is_client 是否为 client
 * @return 0 成功 / -1 失败(命令错误/类型不支持/连接失败) / -5 无内存
 * @details ConID 显式指定 = socket 下标+1(第 5 参, 真机确认生效;
 *          SOCKETDEL 后同一 ConID 可复用); 成功响应 "connect success
 *          ConID=<n>" 行 + OK(有空格, 真机确认); 失败直接重试一次
 *          (失败时连接未建立, 无 ConID 可 SOCKETDEL)
 */
static int aiwb2_socket_connect(struct at_socket* socket, char* ip, int32_t port, enum at_socket_type type, rt_bool_t is_client)
{
    int result = RT_EOK;
    rt_bool_t retryed = RT_FALSE;
    at_response_t resp = RT_NULL;
    int device_socket = (int)(intptr_t)socket->user_data;
    int con_id = device_socket + 1;
    struct at_device* device = (struct at_device*)socket->device;

    RT_ASSERT(ip);
    RT_ASSERT(port >= 0);

    resp = at_create_resp(128, 0, 30 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

__retry:
    if (is_client) {
        switch (type) {
        case AT_SOCKET_TCP:
            /* AT+SOCKET=4,<host>,<port>,0,<conID>(keepalive 占位 0, Combo 未实现) */
            if (at_obj_exec_cmd(device->client, resp, "AT+SOCKET=4,\"%s\",%d,0,%d", ip, port, con_id) < 0) {
                result = -RT_ERROR;
            }
            break;

        case AT_SOCKET_UDP:
            if (at_obj_exec_cmd(device->client, resp, "AT+SOCKET=2,\"%s\",%d,0,%d", ip, port, con_id) < 0) {
                result = -RT_ERROR;
            }
            break;

        default:
            LOG_E("not supported connect type %d.", type);
            result = -RT_ERROR;
            goto __exit;
        }
    }

    if (result == RT_EOK) {
        /* 校验实际分配 ConID 与指定一致: "connect success ConID=<n>" */
        int resp_con_id = -1;
        rt_size_t i;

        for (i = 1; i <= resp->line_counts; i++) {
            const char* line = at_resp_get_line(resp, i);

            if (line != RT_NULL && at_resp_parse_line_args(resp, i, "connect success ConID=%d", &resp_con_id) > 0) {
                break;
            }
        }
        if (resp_con_id == con_id) {
            socket->user_data = (void*)(intptr_t)con_id;
        }
        else {
            LOG_E("%s device socket(%d) connect failed, got ConID=%d.", device->name, device_socket, resp_con_id);
            result = -RT_ERROR;
        }
    }

    if (result != RT_EOK && retryed == RT_FALSE) {
        LOG_D("%s device socket (%d) connect failed, retry.", device->name, device_socket);
        retryed = RT_TRUE;
        result = RT_EOK;
        goto __retry;
    }

__exit:
    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * @brief 发送数据, AT+SOCKETSEND=<ConID>,<len>, 1024B 分片
 * @param socket 当前 at_socket 对象
 * @param buff   发送缓冲
 * @param bfsz   发送长度
 * @param type   socket 类型(未使用)
 * @return >=0 发送成功长度 / -1 失败 / -2 超时 / -5 无内存
 * @details ① 发指令等 '>' 提示符(line_num=1 + end_sign='>' 兜底, '>' 行到达即
 *          返回); ② 发数据; ③ 等 OK/ERROR 命令响应(aiwb2_send_wait)。
 *          ConID 不存在时模组静默丢弃无响应(真机确认) → send_wait 超时,
 *          上层按发送失败处理并由其重连逻辑恢复。
 */
static int aiwb2_socket_send(struct at_socket* socket, const char* buff, size_t bfsz, enum at_socket_type type)
{
    int result = RT_EOK;
    size_t cur_pkt_size = 0, sent_size = 0;
    at_response_t resp = RT_NULL;
    int con_id = (int)(intptr_t)socket->user_data;
    struct at_device* device = (struct at_device*)socket->device;
    rt_mutex_t lock = at_device_get_client_lock(device);

    RT_ASSERT(buff);
    RT_ASSERT(bfsz > 0);
    RT_UNUSED(type);

    rt_mutex_take(lock, RT_WAITING_FOREVER);

    /* set AT client end sign to deal with '>' sign(兼容无 \r\n 的提示符) */
    at_obj_set_end_sign(device->client, '>');

    while (sent_size < bfsz) {
        if (bfsz - sent_size < AIWB2_MODULE_SEND_MAX_SIZE) {
            cur_pkt_size = bfsz - sent_size;
        }
        else {
            cur_pkt_size = AIWB2_MODULE_SEND_MAX_SIZE;
        }

        /* ① 发指令等 '>' 提示符: line_num=1, 收到 1 行(即 '>')即返回 */
        resp = at_create_resp(64, 1, 5 * RT_TICK_PER_SECOND);
        if (resp == RT_NULL) {
            result = -RT_ENOMEM;
            goto __exit;
        }
        if (at_obj_exec_cmd(device->client, resp, "AT+SOCKETSEND=%d,%d", con_id, cur_pkt_size) < 0) {
            result = -RT_ERROR;
            goto __exit;
        }
        at_delete_resp(resp);
        resp = RT_NULL;

        /* ② 发送真实数据 */
        if (at_client_obj_send(device->client, buff + sent_size, cur_pkt_size) == 0) {
            result = -RT_ERROR;
            goto __exit;
        }

        /* ③ 等待 OK/ERROR 命令响应(数据已在 ② 发出, 仅等响应行) */
        result = aiwb2_send_wait(device, AIWB2_SEND_WAIT_TIMEOUT);
        if (result != RT_EOK) {
            goto __exit;
        }

        sent_size += cur_pkt_size;
    }

    result = (int)sent_size; /* 全部发送成功, 返回发送总长 */

__exit:
    /* reset the end sign for data conflict */
    at_obj_set_end_sign(device->client, 0);

    rt_mutex_release(lock);

    if (resp) {
        at_delete_resp(resp);
    }

    return result > 0 ? (int)sent_size : result;
}

/**
 * @brief 域名解析, AT+WDOMAIN=<name> → +WDOMAIN:<IP> + OK
 * @param name 域名
 * @param ip   解析结果 IP(长度至少 16)
 * @return 0 成功 / -1 失败 / -2 超时 / -5 无内存
 * @note 仅作为 SAL getaddrinfo 兜底回调; 连接主路径为 SOCKET 直写域名,
 *       省一次解析往返。真机确认 WDOMAIN 仍存在(固件 V4.18_P23.2.1),
 *       官方 skill 的"已删除"说法与真机不符
 */
static int aiwb2_domain_resolve(const char* name, char ip[16])
{
#define RESOLVE_RETRY 5

    int i, result = RT_EOK;
    char recv_ip[16] = {0};
    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;

    RT_ASSERT(name);
    RT_ASSERT(ip);

    device = at_device_get_first_initialized();
    if (device == RT_NULL) {
        LOG_E("get first init device failed.");
        return -RT_ERROR;
    }

    resp = at_create_resp(128, 0, 20 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    for (i = 0; i < RESOLVE_RETRY; i++) {
        if (at_obj_exec_cmd(device->client, resp, "AT+WDOMAIN=%s", name) < 0) {
            result = -RT_ERROR;
            goto __exit;
        }

        if (at_resp_parse_line_args_by_kw(resp, "+WDOMAIN:", "+WDOMAIN:%[^\r\n]", recv_ip) <= 0) {
            rt_thread_mdelay(100);
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }

        if (rt_strlen(recv_ip) < 8) {
            rt_thread_mdelay(100);
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }
        else {
            rt_strncpy(ip, recv_ip, 15);
            ip[15] = '\0';
            result = RT_EOK;
            break;
        }
    }
    if (rt_strlen(recv_ip) < 8) {
        result = -RT_ERROR; /* 重试耗尽仍未解析成功 */
    }

__exit:
    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * @brief 设置 AT socket 事件通知回调
 * @param event 通知事件
 * @param cb    通知回调
 */
static void aiwb2_socket_set_event_cb(at_socket_evt_t event, at_evt_cb_t cb)
{
    if ((rt_size_t)event < sizeof(at_evt_cb_set) / sizeof(at_evt_cb_set[1])) {
        at_evt_cb_set[event] = cb;
    }
}

static const struct at_socket_ops aiwb2_socket_ops = {
    aiwb2_socket_connect,
    aiwb2_socket_close,
    aiwb2_socket_send,
    aiwb2_domain_resolve,
    aiwb2_socket_set_event_cb,
    RT_NULL, /* at_socket(本驱动不主动分配) */
#ifdef AT_USING_SOCKET_SERVER
    RT_NULL, /* listen 未实现(工程仅 client), 需要时按 AT+SOCKET=3,<port> 实现 */
#endif
};

/**
 * @brief 接收数据 URC, +EVENT:SocketDown,<ConID>,<len>[,<data>]
 * @details 主动模式(SOCKETRECVCFG=1)数据在行内, len 为真实字节数; 被动模式
 *          (=0)无数据内容。数据含 \r\n 序列会被 at_client 提前断行导致丢包
 *          (at_client.c 已知限制), 工程数据(MQTT 紧凑 JSON)出现概率低。
 */
static void urc_recv_func(struct at_client* client, const char* data, rt_size_t size)
{
    int con_id = 0;
    rt_size_t bfsz = 0;
    const char *data_ptr = RT_NULL, *p = RT_NULL;
    struct at_socket* socket = RT_NULL;
    struct at_device* device = RT_NULL;
    char* client_name = client->device->parent.name;

    RT_ASSERT(data && size);

    /* 取 ConID 与长度: +EVENT:SocketDown,<ConID>,<len> */
    if (rt_sscanf(data, "+EVENT:SocketDown,%d,%d", &con_id, (int*)&bfsz) != 2) {
        return;
    }

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", client_name);
        return;
    }

    /* 数据起点 = 第 3 个逗号之后(被动模式无数据, 逗号不存在) */
    p = strchr(data, ',');
    p = p ? strchr(p + 1, ',') : RT_NULL;
    p = p ? strchr(p + 1, ',') : RT_NULL;
    if (p == RT_NULL) {
        return; /* 被动模式事件, 无数据可收 */
    }
    data_ptr = p + 1;

    if (con_id < 0 || bfsz == 0) {
        return;
    }

    /* 防御: 行内可用数据长度裁剪(行尾 \r\n 不计入) */
    {
        rt_size_t avail = size - (data_ptr - data);

        if (avail >= 2 && data[size - 2] == '\r' && data[size - 1] == '\n') {
            avail -= 2;
        }
        if (bfsz > avail) {
            bfsz = avail;
        }
    }

    socket = aiwb2_find_socket(device, con_id);
    if (socket == RT_NULL) {
        LOG_D("%s device socket(ConID=%d) not found, drop %d bytes.", device->name, con_id, (int)bfsz);
        return;
    }

    /* notice the receive buffer and buffer size */
    if (at_evt_cb_set[AT_SOCKET_EVT_RECV]) {
        at_evt_cb_set[AT_SOCKET_EVT_RECV](socket, AT_SOCKET_EVT_RECV, data_ptr, bfsz);
    }
}

/**
 * @brief Socket 断开 URC, +EVENT:SocketDisconnect,<ConID>(标准拼写单 s, 真机确认)
 */
static void urc_close_func(struct at_client* client, const char* data, rt_size_t size)
{
    int con_id = 0;
    struct at_socket* socket = RT_NULL;
    struct at_device* device = RT_NULL;
    char* client_name = client->device->parent.name;

    RT_ASSERT(data && size);
    RT_UNUSED(size);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", client_name);
        return;
    }

    if (rt_sscanf(data, "+EVENT:SocketDisconnect,%d", &con_id) != 1) {
        return;
    }

    socket = aiwb2_find_socket(device, con_id);
    if (socket == RT_NULL) {
        return;
    }

    /* notice the socket is disconnect by remote */
    if (at_evt_cb_set[AT_SOCKET_EVT_CLOSED]) {
        at_evt_cb_set[AT_SOCKET_EVT_CLOSED](socket, AT_SOCKET_EVT_CLOSED, RT_NULL, 0);
    }
}

/**
 * @brief TCP client 断线自动重连成功 URC, +EVENT:SocketReconnect,<ConID>(仅日志)
 * @note 固件自动重连, SAL 层连接状态已被 Disconnect 破坏, paho 自管重连
 */
static void urc_reconn_func(struct at_client* client, const char* data, rt_size_t size)
{
    RT_UNUSED(client);
    RT_UNUSED(data);
    RT_UNUSED(size);
    LOG_D("aiwb2 socket reconnect: %.*s", (int)size, data);
}

/**
 * @brief 连接删除 URC, +EVENT:SocketAutoDel,<ConID>(手动 SOCKETDEL 成功也触发, 仅日志)
 */
static void urc_autodel_func(struct at_client* client, const char* data, rt_size_t size)
{
    RT_UNUSED(client);
    RT_UNUSED(data);
    RT_UNUSED(size);
    LOG_D("aiwb2 socket auto del: %.*s", (int)size, data);
}

#ifdef AT_USING_SOCKET_SERVER
/**
 * @brief TCP Server 新连接 URC, +EVENT:SocketSeed,<seedConID>,<serverConID>
 * @note 工程仅 client, 实现留日志
 */
static void urc_seed_func(struct at_client* client, const char* data, rt_size_t size)
{
    RT_ASSERT(data && size);
    LOG_D("aiwb2 socket seed: %.*s", (int)size, data);
}
#endif

int aiwb2_socket_init(struct at_device* device)
{
    RT_ASSERT(device);

    /* register URC data execution function */
    at_obj_set_urc_table(device->client, aiwb2_socket_urc_table, sizeof(aiwb2_socket_urc_table) / sizeof(aiwb2_socket_urc_table[0]));

    return RT_EOK;
}

int aiwb2_socket_class_register(struct at_device_class* class)
{
    RT_ASSERT(class);

    class->socket_num = AT_DEVICE_AIWB2_SOCKETS_NUM;
    class->socket_ops = &aiwb2_socket_ops;

    return RT_EOK;
}

#endif /* AT_DEVICE_USING_AIWB2 && AT_USING_SOCKET */
