/*
 * Copyright (C) 2022-2024, Xiaohua Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-22     OpenCode     first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "lwrb/lwrb.h"
#include "uart_test.h"

#define LOG_TAG "uart_test"
#define LOG_LVL LOG_LVL_DBG
#include <ulog.h>

/* ---------- Quick configuration: change UART name here ---------- */
#define TEST_UART_NAME     "uart4"

/* Receive ring buffer size. */
#define TEST_RX_BUF_SIZE   512

/* Maximum single TX/RX debug frame size. */
#define TEST_FRAME_SIZE    256

static rt_device_t  uart_test_dev = RT_NULL;
static uint32_t     uart_test_baudrate = 115200;

static lwrb_t       rx_rb;
static uint8_t      rx_rb_data[TEST_RX_BUF_SIZE];
static struct rt_semaphore rx_sem;

/**
 * @brief Convert a hex character to its 4-bit value.
 */
static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief Parse a hex string (e.g. "01 03 00 00 00 0A") into bytes.
 *        Separators (space, comma, dash) are ignored.
 * @return Number of bytes parsed.
 */
static int hex_str_to_bytes(const char *hex, uint8_t *out, uint16_t max_len)
{
    int len = 0;

    if (hex == RT_NULL || out == RT_NULL || max_len == 0)
    {
        return 0;
    }

    while (*hex != '\0' && len < max_len)
    {
        while (*hex == ' ' || *hex == '\t' || *hex == ',' || *hex == '-')
        {
            hex++;
        }
        if (*hex == '\0')
        {
            break;
        }

        int h = hex_char_to_nibble(*hex++);
        if (h < 0)
        {
            break;
        }
        int l = hex_char_to_nibble(*hex++);
        if (l < 0)
        {
            break;
        }

        out[len++] = (uint8_t)((h << 4) | l);
    }

    return len;
}

/**
 * @brief UART RX indicate callback: drain UART FIFO into ring buffer.
 */
static rt_err_t uart_test_rx_ind(rt_device_t dev, rt_size_t size)
{
    uint8_t ch;

    (void)size;

    if (dev == RT_NULL)
    {
        return -RT_ERROR;
    }

    while (rt_device_read(dev, 0, &ch, 1) == 1)
    {
        lwrb_write(&rx_rb, &ch, 1);
    }

    rt_sem_release(&rx_sem);

    return RT_EOK;
}

int uart_test_init(uint32_t baudrate)
{
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;

    if (uart_test_dev != RT_NULL)
    {
        rt_device_close(uart_test_dev);
        uart_test_dev = RT_NULL;
    }

    uart_test_dev = rt_device_find(TEST_UART_NAME);
    if (uart_test_dev == RT_NULL)
    {
        LOG_E("device %s not found", TEST_UART_NAME);
        return -RT_ERROR;
    }

    cfg.baud_rate = baudrate;
    cfg.data_bits = DATA_BITS_8;
    cfg.stop_bits = STOP_BITS_1;
    cfg.parity    = PARITY_NONE;
    cfg.bit_order = BIT_ORDER_LSB;
    cfg.bufsz     = 64;
    cfg.reserved  = 0;

    if (rt_device_control(uart_test_dev, RT_DEVICE_CTRL_CONFIG, &cfg) != RT_EOK)
    {
        LOG_E("config %s failed", TEST_UART_NAME);
        uart_test_dev = RT_NULL;
        return -RT_ERROR;
    }

    if (rt_device_open(uart_test_dev, RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        LOG_E("open %s failed", TEST_UART_NAME);
        uart_test_dev = RT_NULL;
        return -RT_ERROR;
    }

    rt_device_set_rx_indicate(uart_test_dev, uart_test_rx_ind);

    lwrb_init(&rx_rb, rx_rb_data, sizeof(rx_rb_data));
    rt_sem_init(&rx_sem, "uart_rx", 0, RT_IPC_FLAG_FIFO);

    uart_test_baudrate = baudrate;

    LOG_I("init ok, %s baudrate=%lu", TEST_UART_NAME, uart_test_baudrate);
    return RT_EOK;
}

int uart_test_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (uart_test_dev == RT_NULL || data == RT_NULL || len == 0)
    {
        return -RT_ERROR;
    }

    rt_device_write(uart_test_dev, 0, data, len);

    return (int)len;
}

int uart_test_recv(uint8_t *buf, uint16_t max_len, uint32_t timeout_ms)
{
    rt_err_t err;
    lwrb_sz_t n;

    if (uart_test_dev == RT_NULL || buf == RT_NULL || max_len == 0)
    {
        return -RT_ERROR;
    }

    err = rt_sem_take(&rx_sem, rt_tick_from_millisecond(timeout_ms));
    if (err != RT_EOK && err != -RT_ETIMEOUT)
    {
        return -RT_ERROR;
    }

    rt_base_t level = rt_hw_interrupt_disable();
    n = lwrb_read(&rx_rb, buf, max_len);
    rt_hw_interrupt_enable(level);

    return (int)n;
}

int uart_test_send_then_recv(const uint8_t *tx_buf, uint16_t tx_len,
                             uint8_t *rx_buf, uint16_t max_rx_len,
                             uint32_t timeout_ms)
{
    int ret;

    ret = uart_test_send(tx_buf, tx_len, timeout_ms);
    if (ret < 0)
    {
        return ret;
    }

    return uart_test_recv(rx_buf, max_rx_len, timeout_ms);
}

/**
 * @brief MSH command: send hex string over UART and print the response.
 *
 * Usage:
 *   uart_test 01 03 00 00 00 0A C5 CD
 *   uart_test 01030000000AC5CD
 */
static void uart_test_cmd(int argc, char **argv)
{
    uint8_t tx_buf[TEST_FRAME_SIZE];
    uint8_t rx_buf[TEST_FRAME_SIZE];
    int tx_len;
    int rx_len;
    int i;

    if (argc < 2)
    {
        rt_kprintf("usage: uart_test <hex bytes>\n");
        rt_kprintf("   eg: uart_test 01 03 00 00 00 0A C5 CD\n");
        return;
    }

    char hex_str[256] = {0};
    for (i = 1; i < argc; i++)
    {
        size_t used = rt_strlen(hex_str);
        size_t remain = sizeof(hex_str) - used - 1;
        if (remain > 0)
        {
            rt_strncpy(hex_str + used, argv[i], remain);
            hex_str[sizeof(hex_str) - 1] = '\0';
        }
        if (i < argc - 1)
        {
            used = rt_strlen(hex_str);
            remain = sizeof(hex_str) - used - 1;
            if (remain > 0)
            {
                rt_strncpy(hex_str + used, " ", remain);
                hex_str[sizeof(hex_str) - 1] = '\0';
            }
        }
    }

    tx_len = hex_str_to_bytes(hex_str, tx_buf, sizeof(tx_buf));
    if (tx_len <= 0)
    {
        rt_kprintf("invalid hex string\n");
        return;
    }

    rt_kprintf("TEST TX[%d]:", tx_len);
    for (i = 0; i < tx_len; i++)
    {
        rt_kprintf(" %02X", tx_buf[i]);
    }
    rt_kprintf("\n");

    rx_len = uart_test_send_then_recv(tx_buf, (uint16_t)tx_len, rx_buf, sizeof(rx_buf), 200);
    if (rx_len < 0)
    {
        rt_kprintf("TEST send/recv failed: %d\n", rx_len);
        return;
    }

    rt_kprintf("TEST RX[%d]:", rx_len);
    for (i = 0; i < rx_len; i++)
    {
        rt_kprintf(" %02X", rx_buf[i]);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(uart_test_cmd, send hex data to UART and print response);

/**
 * @brief Auto-initialize UART test with default baudrate.
 */
static int uart_test_auto_init(void)
{
    return uart_test_init(115200);
}
//INIT_APP_EXPORT(uart_test_auto_init);
