/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-04     opencode     first version (Ai-WB2 Combo-AT)
 */

#ifndef __AT_DEVICE_AIWB2_H__
#define __AT_DEVICE_AIWB2_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

#include <at_device.h>

/**
 * @defgroup AIWB2 Ai-WB2 AT 驱动
 * @brief 安信可 Ai-WB2(BL602) Combo-AT 固件驱动，替代旧 ESP8266 AT 固件
 * @details 指令权威参考: docs/esp8266_combo_at_commands.md(真机回填版)。
 *          类 ID 在官方 at_device.h 枚举(至 0x17 ML307)之外自定义 0x18,
 *          官方头文件不做修改。
 * @{
 */

/**
 * @name Ai-WB2 设备类 ID
 * @{
 */
#define AT_DEVICE_CLASS_AIWB2 0x18U
/** @} */

/**
 * @name Ai-WB2 驱动常量
 * @{
 */
/** 模块最大 socket 数(工程仅 paho 1 个 TCP client, 5 保守够用) */
#define AT_DEVICE_AIWB2_SOCKETS_NUM 5

/** 发送分片阈值(Combo 真机确认 1024B, 旧 ESP8266 为 2048B) */
#define AIWB2_MODULE_SEND_MAX_SIZE 1024
/** @} */

/**
 * @brief Ai-WB2 AT 设备配置结构体
 * @note 结构体指针作为 user_data 传入 at_device_register
 */
struct at_device_aiwb2
{
    char* device_name;       /**< 网卡设备名, 如 "aiwb20" */
    char* client_name;       /**< AT 客户端串口设备名(波特率 115200) */
    char* wifi_ssid;         /**< WiFi SSID(样例指向 g_gw_env.wifi_ssid, 不拷贝) */
    char* wifi_password;     /**< WiFi 密码(样例指向 g_gw_env.wifi_pwd, 不拷贝) */
    size_t recv_line_num;    /**< at_client_init 接收行缓冲长度(SocketDown 数据在行内) */
    struct at_device device; /**< 框架 AT 设备对象 */
    void* user_data;         /**< 保留 */
};

#ifdef AT_USING_SOCKET

/* Ai-WB2 device socket initialize */
int aiwb2_socket_init(struct at_device* device);

/* Ai-WB2 device class socket register */
int aiwb2_socket_class_register(struct at_device_class* class);

#endif /* AT_USING_SOCKET */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __AT_DEVICE_AIWB2_H__ */
