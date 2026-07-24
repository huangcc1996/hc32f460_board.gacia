/*
 * Copyright (C) 2022-2024, Xiaohua Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-04-28     CDT          first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "drv_spi.h"
#include "board_config.h"
#include "fal.h"
#include "spi_flash_sfud.h"

/* defined the LED_GREEN pin: PD4 */
#define LED_GREEN_PIN GET_PIN(D, 10)

//这个变量用于占用0x1FFFFFFC地址，SRAMH 和 SRAM1 的边界不支持非对齐访问，使用上必须避免对
//0x1FFF_FFFD/0x1FFF_FFFE/0x1FFF_FFFF 地址发起 32bit 访问，对 0x1FFF_FFFF 地址发起 16bit
//访问。
static uint32_t _empty __attribute__((at(0x1FFFFFFC)));
extern int fdb_kvdc_params(void);
int main(void)
{
    /* set LED_GREEN_PIN pin mode to output */
    rt_pin_mode(LED_GREEN_PIN, PIN_MODE_OUTPUT);
    
    rt_hw_spi_device_attach("spi3", "spi30", SPI3_CS_PORT, SPI3_CS_PIN);
    if (RT_NULL != rt_sfud_flash_probe("norflash0", "spi30")){
        fal_init();
        fdb_kvdc_params();
    }

    while (1)
    {
        rt_pin_write(LED_GREEN_PIN, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_GREEN_PIN, PIN_LOW);
        rt_thread_mdelay(500);
    }
}

