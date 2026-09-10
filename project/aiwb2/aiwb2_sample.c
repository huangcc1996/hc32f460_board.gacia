/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-09-04     opencode     first version (Ai-WB2 Combo-AT)
 */

#include <at_device_aiwb2.h>

#define LOG_TAG "at.sample.aiwb2"
#include <at_log.h>

#define AIWB2_SAMPLE_DEIVCE_NAME "aiwb20"

#ifndef AIWB2_SAMPLE_CLIENT_NAME
#define AIWB2_SAMPLE_CLIENT_NAME "uart4"
#endif

#ifndef AIWB2_SAMPLE_RECV_BUFF_LEN
#define AIWB2_SAMPLE_RECV_BUFF_LEN 2048
#endif

/* WiFi 凭证运行时取自 g_gw_env(网关 env 参数, flash 可配):
 * aiwb2 驱动仅持指针拼 AT 指令(不拷贝), setenv wifi_ssid/wifi_pwd 后重启生效 */
//#include "gateway_env.h"

static struct at_device_aiwb2 aiwb2_dev = {
    AIWB2_SAMPLE_DEIVCE_NAME,
    AIWB2_SAMPLE_CLIENT_NAME,

    "ruijie", /* wifi_ssid: 由 aiwb2_user_init 指向 g_gw_env.wifi_ssid */
    "Gacia@2025.", /* wifi_password: 由 aiwb2_user_init 指向 g_gw_env.wifi_pwd */
    AIWB2_SAMPLE_RECV_BUFF_LEN,
    {0},     /* struct at_device device(由 at_device_register 填充) */
    RT_NULL, /* user_data 保留 */
};

static int aiwb2_device_register(void)
{
    struct at_device_aiwb2* aiwb2 = &aiwb2_dev;

    return at_device_register(&(aiwb2->device), aiwb2->device_name, aiwb2->client_name, AT_DEVICE_CLASS_AIWB2, (void*)aiwb2);
}

/**
 * @brief Ai-WB2 设备注册入口(BSP 按 net_module=AUTO/WIFI 显式调用)
 * @details Ai-WB2 一般无需外部 EN/RST 控制(串口直连), 无需 GPIO 初始化;
 *          如硬件需复位可在本函数按 BSP 实际 GPIO 加控制时序
 */
#include <board.h>
#include <rtdevice.h>
#include <rtthread.h>
int aiwb2_user_init(void)
{
//    aiwb2_dev.wifi_ssid = g_gw_env.wifi_ssid;
//    aiwb2_dev.wifi_password = g_gw_env.wifi_pwd;
    aiwb2_device_register();
    return RT_EOK;
}
INIT_APP_EXPORT(aiwb2_user_init);
