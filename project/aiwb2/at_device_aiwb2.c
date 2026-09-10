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

#define LOG_TAG "at.dev.aiwb2"

#include <at_log.h>

#ifdef AT_DEVICE_USING_AIWB2

#define AIWB2_WAIT_CONNECT_TIME 5000
#define AIWB2_THREAD_STACK_SIZE 2048
#define AIWB2_THREAD_PRIORITY (RT_THREAD_PRIORITY_MAX / 2)

/* =============================  aiwb2 network interface operations ============================= */

static int aiwb2_netdev_set_dns_server(struct netdev* netdev, uint8_t dns_num, ip_addr_t* dns_server);

/**
 * @brief 按逗号拆分一行响应字段(行尾 '\r'/'\n' 自动剔除)
 * @param line  输入行
 * @param field 输出字段数组(至少 max 个元素)
 * @param max   字段数组容量
 * @return 实际字段数(<= max), 0 表示无字段
 */
static int aiwb2_split_fields(const char* line, char field[][32], int max)
{
    int n = 0;
    const char* p = line;

    if (line == RT_NULL || *line == '\0') {
        return 0;
    }

    while (n < max) {
        int len = 0;

        while (*p && *p != ',' && *p != '\r' && *p != '\n' && len < 31) {
            field[n][len++] = *p++;
        }
        field[n][len] = '\0';
        n++;

        if (*p == ',') {
            p++;
            continue;
        }
        if (len == 31) {
            /* 字段超长被截断: 跳过剩余字符至分隔符, 保持后续字段对齐 */
            while (*p && *p != ',' && *p != '\r' && *p != '\n') {
                p++;
            }
            if (*p == ',') {
                p++;
                continue;
            }
        }
        break;
    }

    return n;
}

/**
 * @brief 采集 Ai-WB2 网络信息(延时工作线程), 异步填充 netdev
 * @param work      延时工作对象(本次创建, 需释放)
 * @param work_data 设备对象(struct at_device*)
 * @note 指令与解析格式均真机确认(2026-09-04):
 *       STAINFO? 三行结构(+STAINFO:<status> / SSID: / Password: / 逗号字段行),
 *       MAC 带冒号小写; CIPSTAMAC_DEF? 为 12 位小写 hex 无分隔; WSDHCP? DHCP 模式
 *       仅回 MODE=1 无 IP/MASK/GW; WDNS? 一行两个 IP 未配置项回 0.0.0.0
 * @note STAINFO 的 Password 明文回显, 本函数不打日志内容(脱敏)
 */
static void aiwb2_get_netdev_info(struct rt_work* work, void* work_data)
{
#define AT_ADDR_LEN 32
#define AT_ERR_DNS_SERVER "255.255.255.255"
#define AT_DEF_DNS_SERVER "114.114.114.114"
#define AIWB2_STAINFO_FIELD_MAX 12

    at_response_t resp = RT_NULL;
    char mac[AT_ADDR_LEN] = {0}, ip[AT_ADDR_LEN] = {0};
    char gateway[AT_ADDR_LEN] = {0};
    char dns_server1[AT_ADDR_LEN] = {0}, dns_server2[AT_ADDR_LEN] = {0};
    ip_addr_t ip_addr;
    rt_uint32_t mac_addr[6] = {0};
    rt_uint32_t num = 0;
    int stainfo_status = 0;
    int dhcp_stat = 0;
    struct at_device* device = (struct at_device*)work_data;
    struct netdev* netdev = device->netdev;
    struct at_client* client = device->client;

    if (work != RT_NULL) {
        rt_free(work);
    }

    resp = at_create_resp(512, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return;
    }

    /* ① 查询 STA 连接信息 "AT+STAINFO?"(推荐, 状态/IP/gateway/MAC 一次拿齐) */
    if (at_obj_exec_cmd(client, resp, "AT+STAINFO?") < 0) {
        LOG_W("%s device send \"AT+STAINFO?\" cmd error.", device->name);
        goto __exit;
    }

    /* 解析 +STAINFO:<status> 行与逗号字段行
     * 逗号行: <bssid>,<Security>,<MAC>,<ch>,<IP>,<gateway>
     * (从行尾倒数取 gateway/IP/ch/MAC, 规避 SSID/Security 含逗号时字段数变化) */
    {
        char field[AIWB2_STAINFO_FIELD_MAX][32] = {{0}};
        int n = 0;
        rt_size_t i;

        for (i = 1; i <= resp->line_counts; i++) {
            const char* line = at_resp_get_line(resp, i);

            if (line == RT_NULL) {
                continue;
            }
            if (at_resp_parse_line_args(resp, i, "+STAINFO:%d", &stainfo_status) > 0) {
                continue;
            }
            if (strstr(line, "SSID:") == line || strstr(line, "Password:") == line) {
                continue; /* SSID:/Password: 行跳过(Password 明文不处理) */
            }
            n = aiwb2_split_fields(line, field, AIWB2_STAINFO_FIELD_MAX);
            if (n >= 6) {
                break;
            }
        }

        /* 仅 status=3(已连已获 IP) 时采集, 其余状态保持旧值 */
        if (n >= 6 && stainfo_status == 3) {
            rt_snprintf(gateway, sizeof(gateway), "%s", field[n - 1]);
            rt_snprintf(ip, sizeof(ip), "%s", field[n - 2]);
            /* field[n-3] = ch, 本驱动不采集 */
            if (strchr(field[n - 4], ':') != RT_NULL) {
                rt_snprintf(mac, sizeof(mac), "%s", field[n - 4]);
            }
        }
    }

    if (ip[0] == '\0') {
        /* ② 备选: 查询 STA MAC "AT+CIPSTAMAC_DEF?"(12 位小写 hex 无分隔) */
        if (at_obj_exec_cmd(client, resp, "AT+CIPSTAMAC_DEF?") < 0) {
            LOG_W("%s device send \"AT+CIPSTAMAC_DEF?\" cmd error.", device->name);
        }
        else if (at_resp_parse_line_args_by_kw(resp, "+CIPSTAMAC_DEF:", "+CIPSTAMAC_DEF:%[^\r\n]", mac) <= 0) {
            LOG_W("%s device parse \"AT+CIPSTAMAC_DEF?\" cmd error.", device->name);
        }
    }

    /* ③ 查询 DHCP 状态 "AT+WSDHCP?" */
    if (at_obj_exec_cmd(client, resp, "AT+WSDHCP?") < 0) {
        LOG_W("%s device send \"AT+WSDHCP?\" cmd error.", device->name);
        goto __exit;
    }
    if (at_resp_parse_line_args_by_kw(resp, "+WSDHCP:", "+WSDHCP:%d", &dhcp_stat) <= 0) {
        LOG_W("%s device parse \"AT+WSDHCP?\" cmd error.", device->name);
        goto __exit;
    }

    /* ④ 查询 DNS "AT+WDNS?"(一行两个 IP, 未配置项回 0.0.0.0) */
    if (at_obj_exec_cmd(client, resp, "AT+WDNS?") < 0) {
        LOG_W("%s device send \"AT+WDNS?\" cmd error.", device->name);
        goto __exit;
    }
    if (at_resp_parse_line_args_by_kw(resp, "+WDNS:", "+WDNS:%[^,],%[^,\r\n]", dns_server1, dns_server2) <= 0) {
        LOG_W("%s device parse \"AT+WDNS?\" cmd error.", device->name);
        goto __exit;
    }

    /* 设置 netdev 信息(IP/gateway 仅在 STAINFO 采集成功时有效; netmask DHCP 模式无来源, 255.255.255.0 兜底) */
    if (ip[0] != '\0') {
        inet_aton(gateway, &ip_addr);
        netdev_low_level_set_gw(netdev, &ip_addr);
        inet_aton(ip, &ip_addr);
        netdev_low_level_set_ipaddr(netdev, &ip_addr);
    }
    inet_aton("255.255.255.0", &ip_addr);
    netdev_low_level_set_netmask(netdev, &ip_addr);

    if (mac[0] != '\0') {
        if (strchr(mac, ':') != RT_NULL) {
            /* STAINFO MAC 带冒号: 28:bb:ed:43:78:e1 */
            rt_sscanf(mac, "%x:%x:%x:%x:%x:%x", &mac_addr[0], &mac_addr[1], &mac_addr[2], &mac_addr[3], &mac_addr[4], &mac_addr[5]);
        }
        else {
            /* CIPSTAMAC_DEF MAC 12 位 hex 无分隔: 28bbed4378e1, 两两转字节 */
            int j;

            for (j = 0; j < 6 && mac[2 * j] && mac[2 * j + 1]; j++) {
                char hex[3] = {mac[2 * j], mac[2 * j + 1], '\0'};
                mac_addr[j] = (rt_uint32_t)strtol(hex, RT_NULL, 16);
            }
        }
        for (num = 0; num < netdev->hwaddr_len; num++) {
            netdev->hwaddr[num] = mac_addr[num];
        }
    }

    /* 设置 DNS(0.0.0.0/255.255.255.255 无效时回退默认) */
    if (rt_strlen(dns_server1) > 0 && rt_strncmp(dns_server1, AT_ERR_DNS_SERVER, rt_strlen(AT_ERR_DNS_SERVER)) != 0 && rt_strncmp(dns_server1, "0.0.0.0", 7) != 0) {
        inet_aton(dns_server1, &ip_addr);
        netdev_low_level_set_dns_server(netdev, 0, &ip_addr);
    }
    else {
        inet_aton(AT_DEF_DNS_SERVER, &ip_addr);
        aiwb2_netdev_set_dns_server(netdev, 0, &ip_addr);
    }
    if (rt_strlen(dns_server2) > 0 && rt_strncmp(dns_server2, "0.0.0.0", 7) != 0) {
        inet_aton(dns_server2, &ip_addr);
        netdev_low_level_set_dns_server(netdev, 1, &ip_addr);
    }

    /* 设置 DHCP 状态(真机 DHCP 模式 +WSDHCP:1) */
    netdev_low_level_set_dhcp_status(netdev, dhcp_stat ? RT_TRUE : RT_FALSE);

__exit:
    if (resp) {
        at_delete_resp(resp);
    }
}

static int aiwb2_net_init(struct at_device* device);

static int aiwb2_netdev_set_up(struct netdev* netdev)
{
    struct at_device* device = RT_NULL;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    if (device->is_init == RT_FALSE) {
        aiwb2_net_init(device);
        netdev_low_level_set_status(netdev, RT_TRUE);
        LOG_D("network interface device(%s) set up status", netdev->name);
    }

    return RT_EOK;
}

static int aiwb2_netdev_set_down(struct netdev* netdev)
{
    struct at_device* device = RT_NULL;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device by netdev(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    if (device->is_init == RT_TRUE) {
        device->is_init = RT_FALSE;
        netdev_low_level_set_status(netdev, RT_FALSE);
        LOG_D("network interface device(%s) set down status", netdev->name);
    }

    return RT_EOK;
}

/**
 * @brief 设置静态 IP 信息, 合并旧 CIPSTA+CWDHCP 为 AT+WSDHCP=0,<ip>,<mask>,<gw>
 * @note 真机未测静态设置(2026-09-04 仅测 DHCP 查询), 按文档 3.3 格式实现, 真机首测项
 */
static int aiwb2_netdev_set_addr_info(struct netdev* netdev, ip_addr_t* ip_addr, ip_addr_t* netmask, ip_addr_t* gw)
{
#define IPADDR_RESP_SIZE 128
#define IPADDR_SIZE 16

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;
    char ip_str[IPADDR_SIZE] = {0};
    char gw_str[IPADDR_SIZE] = {0};
    char netmask_str[IPADDR_SIZE] = {0};

    RT_ASSERT(netdev);
    RT_ASSERT(ip_addr || netmask || gw);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(IPADDR_RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        result = -RT_ENOMEM;
        goto __exit;
    }

    /* Convert numeric IP address into decimal dotted ASCII representation. */
    if (ip_addr) {
        rt_memcpy(ip_str, inet_ntoa(*ip_addr), IPADDR_SIZE);
    }
    else {
        rt_memcpy(ip_str, inet_ntoa(netdev->ip_addr), IPADDR_SIZE);
    }

    if (gw) {
        rt_memcpy(gw_str, inet_ntoa(*gw), IPADDR_SIZE);
    }
    else {
        rt_memcpy(gw_str, inet_ntoa(netdev->gw), IPADDR_SIZE);
    }

    if (netmask) {
        rt_memcpy(netmask_str, inet_ntoa(*netmask), IPADDR_SIZE);
    }
    else {
        rt_memcpy(netmask_str, inet_ntoa(netdev->netmask), IPADDR_SIZE);
    }

    /* 静态地址: AT+WSDHCP=0,<ip>,<mask>,<gw>(参数无引号, 顺序与旧 CIPSTA 不同) */
    if (at_obj_exec_cmd(device->client, resp, "AT+WSDHCP=0,%s,%s,%s", ip_str, netmask_str, gw_str) < 0) {
        LOG_E("%s device set address failed.", device->name);
        result = -RT_ERROR;
    }
    else {
        /* Update netdev information */
        if (ip_addr) {
            netdev_low_level_set_ipaddr(netdev, ip_addr);
        }

        if (gw) {
            netdev_low_level_set_gw(netdev, gw);
        }

        if (netmask) {
            netdev_low_level_set_netmask(netdev, netmask);
        }

        LOG_D("%s device set address success.", device->name);
    }

__exit:
    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * @brief 设置 DNS 服务器, AT+WDNS=<ip>(设置命令回显 +WDNS: 行 + OK, 解析器兼容)
 */
static int aiwb2_netdev_set_dns_server(struct netdev* netdev, uint8_t dns_num, ip_addr_t* dns_server)
{
#define DNS_RESP_SIZE 128

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;

    RT_ASSERT(netdev);
    RT_ASSERT(dns_server);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device by netdev(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(DNS_RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    /* AT+WDNS=<ip>(无引号) */
    if (at_obj_exec_cmd(device->client, resp, "AT+WDNS=%s", inet_ntoa(*dns_server)) < 0) {
        LOG_E("%s device set DNS failed.", device->name);
        result = -RT_ERROR;
    }
    else {
        netdev_low_level_set_dns_server(netdev, dns_num, dns_server);
        LOG_D("%s device set DNS(%s) success.", device->name, inet_ntoa(*dns_server));
    }

    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * @brief 设置 DHCP 状态, 使能 AT+WSDHCP=1; 禁用 AT+WSDHCP=0,<ip>,<mask>,<gw>
 * @note 禁用时必须带三参(与旧 esp8266 的 AT+CWDHCP=<en>,1 不同), 用 netdev 当前值拼指令
 */
static int aiwb2_netdev_set_dhcp(struct netdev* netdev, rt_bool_t is_enabled)
{
#define RESP_SIZE 128
#define IPADDR_SIZE 16

    int result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;
    char ip_str[IPADDR_SIZE] = {0};
    char gw_str[IPADDR_SIZE] = {0};
    char netmask_str[IPADDR_SIZE] = {0};

    RT_ASSERT(netdev);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device by netdev(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(RESP_SIZE, 0, rt_tick_from_millisecond(300));
    if (resp == RT_NULL) {
        LOG_E("no memory for resp struct.");
        return -RT_ENOMEM;
    }

    if (is_enabled) {
        /* 使能 DHCP */
        if (at_obj_exec_cmd(device->client, resp, "AT+WSDHCP=1") < 0) {
            LOG_E("%s device set DHCP status(%d) failed.", device->name, is_enabled);
            result = -RT_ERROR;
            goto __exit;
        }
    }
    else {
        /* 禁用 DHCP 用当前 netdev 地址转静态 */
        rt_memcpy(ip_str, inet_ntoa(netdev->ip_addr), IPADDR_SIZE);
        rt_memcpy(gw_str, inet_ntoa(netdev->gw), IPADDR_SIZE);
        rt_memcpy(netmask_str, inet_ntoa(netdev->netmask), IPADDR_SIZE);

        if (at_obj_exec_cmd(device->client, resp, "AT+WSDHCP=0,%s,%s,%s", ip_str, netmask_str, gw_str) < 0) {
            LOG_E("%s device set DHCP status(%d) failed.", device->name, is_enabled);
            result = -RT_ERROR;
            goto __exit;
        }
    }

    netdev_low_level_set_dhcp_status(netdev, is_enabled);
    LOG_D("%s device set DHCP status(%d) ok.", device->name, is_enabled);

__exit:
    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

#ifdef NETDEV_USING_PING
/**
 * @brief Ping 测试, AT+PING=<host>(host 可域名直写, 真机确认)
 * @note 成功响应 +PING:<time> + OK; 失败响应 +PING:106 + ERROR(行内含 "ERROR" 匹配失败)
 * @note 失败耗时约 12s(count3 x 4s 超时), 命令超时必须 >12s, 本驱动取 20s
 */
static int aiwb2_netdev_ping(struct netdev* netdev, const char* host, size_t data_len, uint32_t timeout, struct netdev_ping_resp* ping_resp
#if RT_VER_NUM >= 0x50100
                             ,
                             rt_bool_t is_bind
#endif
)
{
#define AIWB2_PING_RESP_TIMEOUT 20 * RT_TICK_PER_SECOND

    rt_err_t result = RT_EOK;
    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;
    int req_time = 0;

#if RT_VER_NUM >= 0x50100
    RT_UNUSED(is_bind);
#endif
    RT_UNUSED(timeout);

    RT_ASSERT(netdev);
    RT_ASSERT(host);
    RT_ASSERT(ping_resp);

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", netdev->name);
        return -RT_ERROR;
    }

    resp = at_create_resp(64, 0, AIWB2_PING_RESP_TIMEOUT);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    /* 域名/IP 直写(无引号), 无需前置解析 */
    if (at_obj_exec_cmd(device->client, resp, "AT+PING=%s", host) < 0) {
        result = -RT_ERROR;
        goto __exit;
    }

    /* 成功: +PING:<time> */
    if (at_resp_parse_line_args_by_kw(resp, "+PING:", "+PING:%d", &req_time) <= 0) {
        result = -RT_ERROR;
        goto __exit;
    }

    if (req_time) {
        ping_resp->data_len = data_len;
        ping_resp->ttl = 0;
        ping_resp->ticks = req_time;
    }

__exit:
    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}
#endif /* NETDEV_USING_PING */

#ifdef NETDEV_USING_NETSTAT
/**
 * @brief 打印网络连接信息, AT+SOCKET?(无 +SOCKET: 前缀, 多连接每行一条)
 * @note 行格式 <ConID>,<type>,<status>,<host>,<rport>,<lport>,<sConID>;
 *       TCPServer 行 host/remote port 为空(3,3,3,,-1,1883,0); 无连接仅回 OK
 * @note 手写逗号 split(sscanf %[^,] 遇空字段会失败), 跳过非 7 字段行
 */
void aiwb2_netdev_netstat(struct netdev* netdev)
{
#define AIWB2_NETSTAT_RESP_SIZE 512
#define AIWB2_NETSTAT_FIELD_MAX 8

    at_response_t resp = RT_NULL;
    struct at_device* device = RT_NULL;
    char field[AIWB2_NETSTAT_FIELD_MAX][32] = {{0}};
    int n;
    rt_size_t i;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_NETDEV, netdev->name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", netdev->name);
        return;
    }

    resp = at_create_resp(AIWB2_NETSTAT_RESP_SIZE, 0, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return;
    }

    /* AT+SOCKET? */
    if (at_obj_exec_cmd(device->client, resp, "AT+SOCKET?") < 0) {
        goto __exit;
    }

    for (i = 1; i <= resp->line_counts; i++) {
        const char* line = at_resp_get_line(resp, i);

        if (line == RT_NULL || strstr(line, "OK") == line) {
            continue;
        }
        n = aiwb2_split_fields(line, field, AIWB2_NETSTAT_FIELD_MAX);
        if (n != 7) {
            continue; /* 跳过非 7 字段行(如粘连/异常行) */
        }
        LOG_RAW("%s: %s:%s ==> %s:%s\n", field[1], inet_ntoa(netdev->ip_addr), field[5], field[3], field[4]);
    }

__exit:
    if (resp) {
        at_delete_resp(resp);
    }
}
#endif /* NETDEV_USING_NETSTAT */

static const struct netdev_ops aiwb2_netdev_ops = {
    aiwb2_netdev_set_up,
    aiwb2_netdev_set_down,

    aiwb2_netdev_set_addr_info,
    aiwb2_netdev_set_dns_server,
    aiwb2_netdev_set_dhcp,

#ifdef NETDEV_USING_PING
    aiwb2_netdev_ping,
#endif
#ifdef NETDEV_USING_NETSTAT
    aiwb2_netdev_netstat,
#endif
    RT_NULL, /* set_default(本驱动不做默认网卡切换) */
};

static struct netdev* aiwb2_netdev_add(const char* netdev_name)
{
#define ETHERNET_MTU 1500
#define HWADDR_LEN 6
    struct netdev* netdev = RT_NULL;

    RT_ASSERT(netdev_name);

    netdev = netdev_get_by_name(netdev_name);
    if (netdev != RT_NULL) {
        return (netdev);
    }

    netdev = (struct netdev*)rt_calloc(1, sizeof(struct netdev));
    if (netdev == RT_NULL) {
        LOG_E("no memory for netdev create.");
        return RT_NULL;
    }

    netdev->mtu = ETHERNET_MTU;
    netdev->ops = &aiwb2_netdev_ops;
    netdev->hwaddr_len = HWADDR_LEN;

#ifdef SAL_USING_AT
    extern int sal_at_netdev_set_pf_info(struct netdev * netdev);
    /* set the network interface socket/netdb operations */
    sal_at_netdev_set_pf_info(netdev);
#endif

    netdev_register(netdev, netdev_name, RT_NULL);

    return netdev;
}

/* =============================  aiwb2 device operations ============================= */

#define AT_SEND_CMD(client, resp, cmd)                                                                                                                                                                 \
    do {                                                                                                                                                                                               \
        (resp) = at_resp_set_info((resp), 256, 0, 5 * RT_TICK_PER_SECOND);                                                                                                                             \
        if (at_obj_exec_cmd((client), (resp), (cmd)) < 0) {                                                                                                                                            \
            result = -RT_ERROR;                                                                                                                                                                        \
            goto __exit;                                                                                                                                                                               \
        }                                                                                                                                                                                              \
    } while (0)

static void aiwb2_netdev_start_delay_work(struct at_device* device)
{
    struct rt_work* net_work = RT_NULL;
    net_work = (struct rt_work*)rt_calloc(1, sizeof(struct rt_work));
    if (net_work == RT_NULL) {
        return;
    }

    rt_work_init(net_work, aiwb2_get_netdev_info, (void*)device);
    rt_work_submit(net_work, RT_TICK_PER_SECOND);
}

static void aiwb2_init_thread_entry(void* parameter)
{
#define INIT_RETRY 5

    struct at_device* device = (struct at_device*)parameter;
    struct at_device_aiwb2* aiwb2 = (struct at_device_aiwb2*)device->user_data;
    struct at_client* client = device->client;
    at_response_t resp = RT_NULL;
    rt_err_t result = RT_EOK;
    rt_size_t i = 0, retry_num = INIT_RETRY;
    rt_bool_t wifi_is_conn = RT_FALSE;

    LOG_D("%s device initialize start.", device->name);

    /* wait aiwb2 device startup finish */
    if (at_client_obj_wait_connect(client, AIWB2_WAIT_CONNECT_TIME)) {
        return;
    }

    resp = at_create_resp(256, 0, 5 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return;
    }

    while (retry_num--) {
        /* reset module */
        AT_SEND_CMD(client, resp, "AT+RST");
        /* reset waiting delay */
        rt_thread_mdelay(1000);
        /* disable echo */
        AT_SEND_CMD(client, resp, "ATE0");
        /* set current mode to Wi-Fi station and save to flash */
        AT_SEND_CMD(client, resp, "AT+WMODE=1,1");
        /* get module version(仅打印, 不再解析版本) */
        AT_SEND_CMD(client, resp, "AT+GMR");
        /* show module version */
        for (i = 0; i < resp->line_counts; i++) {
            LOG_D("%s", at_resp_get_line(resp, i + 1));
        }

        /* initialize successfully */
        result = RT_EOK;
        break;

    __exit:
        if (result != RT_EOK) {
            rt_thread_mdelay(1000);
            LOG_I("%s device initialize retry...", device->name);
        }
    }

    /* connect to WiFi AP, 事件 +EVENT:WIFI_CONNECT 先于 OK 到达(URC 表已注册, 不影响响应) */
    if (at_obj_exec_cmd(client, at_resp_set_info(resp, 128, 0, 30 * RT_TICK_PER_SECOND), "AT+WJAP=\"%s\",\"%s\"", aiwb2->wifi_ssid, aiwb2->wifi_password) != RT_EOK) {
        LOG_W("%s device wifi connect failed, check ssid(%s).", device->name, aiwb2->wifi_ssid);
    }
    else {
        wifi_is_conn = RT_TRUE;
    }

    if (resp) {
        at_delete_resp(resp);
    }

    if (result != RT_EOK) {
        netdev_low_level_set_status(device->netdev, RT_FALSE);
        LOG_E("%s device network initialize failed(%d).", device->name, result);
    }
    else {
        device->is_init = RT_TRUE;
        netdev_low_level_set_status(device->netdev, RT_TRUE);
        if (wifi_is_conn) {
            netdev_low_level_set_link_status(device->netdev, RT_TRUE);
        }
        aiwb2_netdev_start_delay_work(device);
        LOG_I("%s device network initialize successfully.", device->name);
    }
}

static int aiwb2_net_init(struct at_device* device)
{
#ifdef AT_DEVICE_AIWB2_INIT_ASYN
    rt_thread_t tid;

    tid = rt_thread_create("aiwb2_net", aiwb2_init_thread_entry, (void*)device, AIWB2_THREAD_STACK_SIZE, AIWB2_THREAD_PRIORITY, 20);
    if (tid) {
        rt_thread_startup(tid);
    }
    else {
        LOG_E("create %s device init thread failed.", device->name);
        return -RT_ERROR;
    }
#else
    aiwb2_init_thread_entry(device);
#endif /* AT_DEVICE_AIWB2_INIT_ASYN */

    return RT_EOK;
}

static void urc_wifi_func(struct at_client* client, const char* data, rt_size_t size)
{
    struct at_device* device = RT_NULL;

    RT_ASSERT(client && data && size);
    RT_UNUSED(size);
    char* client_name = client->device->parent.name;

    device = at_device_get_by_name(AT_DEVICE_NAMETYPE_CLIENT, client_name);
    if (device == RT_NULL) {
        LOG_E("get device(%s) failed.", client_name);
        return;
    }

    if (rt_strstr(data, "+EVENT:WIFI_CONNECT")) {
        LOG_I("%s device wifi is connected.", device->name);

        if (device->is_init) {
            netdev_low_level_set_link_status(device->netdev, RT_TRUE);
        }
    }
    else if (rt_strstr(data, "+EVENT:WIFI_GOT_IP")) {
        LOG_I("%s device wifi got ip.", device->name);

        if (device->is_init) {
            /* IP 刚获取, 重新触发网络信息采集刷新 netdev */
            netdev_low_level_set_link_status(device->netdev, RT_TRUE);
            aiwb2_netdev_start_delay_work(device);
        }
    }
    else if (rt_strstr(data, "+EVENT:WIFI_DISCONNECT")) {
        LOG_I("%s device wifi is disconnect.", device->name);

        if (device->is_init) {
            netdev_low_level_set_link_status(device->netdev, RT_FALSE);
        }
    }
}

static const struct at_urc urc_table[] = {
    {"+EVENT:WIFI_CONNECT", "\r\n", urc_wifi_func},
    {"+EVENT:WIFI_DISCONNECT", "\r\n", urc_wifi_func},
    {"+EVENT:WIFI_GOT_IP", "\r\n", urc_wifi_func},
};

static int aiwb2_init(struct at_device* device)
{
    struct at_device_aiwb2* aiwb2 = (struct at_device_aiwb2*)device->user_data;

    /* initialize AT client */
#if RT_VER_NUM >= 0x50100
    at_client_init(aiwb2->client_name, aiwb2->recv_line_num, aiwb2->recv_line_num);
#else
    at_client_init(aiwb2->client_name, aiwb2->recv_line_num);
#endif

    device->client = at_client_get(aiwb2->client_name);
    if (device->client == RT_NULL) {
        LOG_E("get AT client(%s) failed.", aiwb2->client_name);
        return -RT_ERROR;
    }

    /* register URC data execution function */
    at_obj_set_urc_table(device->client, urc_table, sizeof(urc_table) / sizeof(urc_table[0]));

#ifdef AT_USING_SOCKET
    aiwb2_socket_init(device);
#endif

    /* add aiwb2 device to the netdev list */
    device->netdev = aiwb2_netdev_add(aiwb2->device_name);
    if (device->netdev == RT_NULL) {
        LOG_E("add netdev(%s) failed.", aiwb2->device_name);
        return -RT_ERROR;
    }

    /* initialize aiwb2 device network */
    return aiwb2_netdev_set_up(device->netdev);
}

static int aiwb2_deinit(struct at_device* device)
{
    return aiwb2_netdev_set_down(device->netdev);
}

/* reset aiwb2 device and initialize device network again */
static int aiwb2_reset(struct at_device* device)
{
    int result = RT_EOK;
    struct at_client* client = device->client;

    /* send "AT+RST" commonds to aiwb2 device */
    result = at_obj_exec_cmd(client, RT_NULL, "AT+RST");
    rt_thread_mdelay(1000);

    /* waiting 10 seconds for aiwb2 device reset */
    device->is_init = RT_FALSE;
    if (at_client_obj_wait_connect(client, AIWB2_WAIT_CONNECT_TIME)) {
        return -RT_ETIMEOUT;
    }

    /* initialize aiwb2 device network */
    aiwb2_net_init(device);

    device->is_init = RT_TRUE;

    return result;
}

/* change aiwb2 wifi ssid and password information */
static int aiwb2_wifi_info_set(struct at_device* device, struct at_device_ssid_pwd* info)
{
    int result = RT_EOK;
    struct at_response* resp = RT_NULL;

    if (info->ssid == RT_NULL || info->password == RT_NULL) {
        LOG_E("input wifi ssid(%s) and password(%s) error.", info->ssid, info->password);
        return -RT_ERROR;
    }

    resp = at_create_resp(128, 0, 30 * RT_TICK_PER_SECOND);
    if (resp == RT_NULL) {
        LOG_E("no memory for resp create.");
        return -RT_ENOMEM;
    }

    /* connect to input wifi ap */
    if (at_obj_exec_cmd(device->client, resp, "AT+WJAP=\"%s\",\"%s\"", info->ssid, info->password) != RT_EOK) {
        LOG_E("%s device wifi connect failed, check ssid(%s).", device->name, info->ssid);
        result = -RT_ERROR;
    }

    if (resp) {
        at_delete_resp(resp);
    }

    return result;
}

static int aiwb2_control(struct at_device* device, int cmd, void* arg)
{
    int result = -RT_ERROR;

    RT_ASSERT(device);

    switch (cmd) {
    case AT_DEVICE_CTRL_POWER_ON:
    case AT_DEVICE_CTRL_POWER_OFF:
    case AT_DEVICE_CTRL_LOW_POWER:
    case AT_DEVICE_CTRL_SLEEP:
    case AT_DEVICE_CTRL_WAKEUP:
    case AT_DEVICE_CTRL_NET_CONN:
    case AT_DEVICE_CTRL_NET_DISCONN:
    case AT_DEVICE_CTRL_GET_SIGNAL:
    case AT_DEVICE_CTRL_GET_GPS:
    case AT_DEVICE_CTRL_GET_VER: LOG_W("not support the control cmd(%d).", cmd); break;
    case AT_DEVICE_CTRL_RESET: result = aiwb2_reset(device); break;
    case AT_DEVICE_CTRL_SET_WIFI_INFO: result = aiwb2_wifi_info_set(device, (struct at_device_ssid_pwd*)arg); break;
    default: LOG_E("input error control cmd(%d).", cmd); break;
    }

    return result;
}

static const struct at_device_ops aiwb2_device_ops = {
    aiwb2_init,
    aiwb2_deinit,
    aiwb2_control,
};

static int aiwb2_device_class_register(void)
{
    struct at_device_class* class = RT_NULL;

    class = (struct at_device_class*)rt_calloc(1, sizeof(struct at_device_class));
    if (class == RT_NULL) {
        LOG_E("no memory for class create.");
        return -RT_ENOMEM;
    }

    /* fill Ai-WB2 device class object */
#ifdef AT_USING_SOCKET
    aiwb2_socket_class_register(class);
#endif
    class->device_ops = &aiwb2_device_ops;

    return at_device_class_register(class, AT_DEVICE_CLASS_AIWB2);
}
INIT_DEVICE_EXPORT(aiwb2_device_class_register);

#endif /* AT_DEVICE_USING_AIWB2 */
