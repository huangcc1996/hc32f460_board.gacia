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
#include "rs485.h"

#define LOG_TAG "rs485"
#define LOG_LVL LOG_LVL_DBG
#include <ulog.h>

/* UART3 is used as the RS485 UART. */
#define RS485_UART_NAME     "uart3"

/* Direction control pin: PA0, high = TX, low = RX. */
#define RS485_DIR_PIN       GET_PIN(A, 0)

/* Receive ring buffer size. */
#define RS485_RX_BUF_SIZE   512

/* Maximum single TX/RX debug frame size. */
#define RS485_FRAME_SIZE    256

static rt_device_t  rs485_dev = RT_NULL;
static uint32_t     rs485_baudrate = 115200;

static lwrb_t       rx_rb;
static uint8_t      rx_rb_data[RS485_RX_BUF_SIZE];
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
        /* Skip separators. */
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
 * @brief Switch RS485 transceiver direction.
 */
static void rs485_dir_tx(void)
{
    rt_pin_write(RS485_DIR_PIN, PIN_HIGH);
}

static void rs485_dir_rx(void)
{
    rt_pin_write(RS485_DIR_PIN, PIN_LOW);
}

/**
 * @brief Wait until all sent bytes have been driven onto the RS485 bus.
 *
 * The waiting time is calculated from baudrate and number of bytes,
 * instead of polling the USART hardware status register.
 *
 * For delays shorter than 1 ms, rt_hw_us_delay() is used for accuracy;
 * for longer delays, rt_thread_mdelay() is used to yield the CPU.
 */
static void rs485_wait_tx_complete(uint16_t len, uint32_t baudrate)
{
    uint32_t total_bits;
    uint32_t delay_us;
    uint32_t delay_ms;

    if (len == 0 || baudrate == 0)
    {
        return;
    }

    /* 8N1: 1 start + 8 data + 1 stop = 10 bits/byte, plus 1 byte margin */
    total_bits = (uint32_t)(len + 1U) * 10U;
    delay_us   = (total_bits * 1000000UL + baudrate - 1UL) / baudrate;

    if (delay_us < 1000U)
    {
        rt_hw_us_delay(delay_us);
    }
    else
    {
        delay_ms = (delay_us + 999U) / 1000U;
        rt_thread_mdelay(delay_ms);
    }
}

/**
 * @brief UART RX indicate callback: drain UART FIFO into ring buffer.
 */
static rt_err_t rs485_rx_ind(rt_device_t dev, rt_size_t size)
{
    uint8_t ch;

    (void)size;

    if (dev == RT_NULL)
    {
        return -RT_ERROR;
    }

    /* Read all currently available bytes into the ring buffer. */
    while (rt_device_read(dev, 0, &ch, 1) == 1)
    {
        lwrb_write(&rx_rb, &ch, 1);
    }

    rt_sem_release(&rx_sem);

    return RT_EOK;
}

int rs485_init(uint32_t baudrate)
{
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;

    if (rs485_dev != RT_NULL)
    {
        rt_device_close(rs485_dev);
        rs485_dev = RT_NULL;
    }

    rs485_dev = rt_device_find(RS485_UART_NAME);
    if (rs485_dev == RT_NULL)
    {
        LOG_E("device %s not found", RS485_UART_NAME);
        return -RT_ERROR;
    }

    cfg.baud_rate = baudrate;
    cfg.data_bits = DATA_BITS_8;
    cfg.stop_bits = STOP_BITS_1;
    cfg.parity    = PARITY_NONE;
    cfg.bit_order = BIT_ORDER_LSB;
    cfg.bufsz     = 64;
    cfg.reserved  = 0;

    if (rt_device_control(rs485_dev, RT_DEVICE_CTRL_CONFIG, &cfg) != RT_EOK)
    {
        LOG_E("config %s failed", RS485_UART_NAME);
        rs485_dev = RT_NULL;
        return -RT_ERROR;
    }

    if (rt_device_open(rs485_dev, RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        LOG_E("open %s failed", RS485_UART_NAME);
        rs485_dev = RT_NULL;
        return -RT_ERROR;
    }

    rt_device_set_rx_indicate(rs485_dev, rs485_rx_ind);

    /* Configure direction control pin, default to receive mode. */
    rt_pin_mode(RS485_DIR_PIN, PIN_MODE_OUTPUT);
    rs485_dir_rx();

    /* Initialize ring buffer and semaphore. */
    lwrb_init(&rx_rb, rx_rb_data, sizeof(rx_rb_data));
    rt_sem_init(&rx_sem, "rs485_rx", 0, RT_IPC_FLAG_FIFO);

    rs485_baudrate = baudrate;

    LOG_I("init ok, baudrate=%lu", rs485_baudrate);
    return RT_EOK;
}

int rs485_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (rs485_dev == RT_NULL || data == RT_NULL || len == 0)
    {
        return -RT_ERROR;
    }

    rs485_dir_tx();

    rt_device_write(rs485_dev, 0, data, len);

    /* Wait for all bits to be driven onto the bus before releasing it. */
    rs485_wait_tx_complete(len, rs485_baudrate);

    rs485_dir_rx();

    return (int)len;
}

int rs485_recv(uint8_t *buf, uint16_t max_len, uint32_t timeout_ms)
{
    rt_err_t err;
    lwrb_sz_t n;

    if (rs485_dev == RT_NULL || buf == RT_NULL || max_len == 0)
    {
        return -RT_ERROR;
    }

    /* Wait for RX indication or timeout. */
    err = rt_sem_take(&rx_sem, rt_tick_from_millisecond(timeout_ms));
    if (err != RT_EOK && err != -RT_ETIMEOUT)
    {
        return -RT_ERROR;
    }

    /* Read from ring buffer with interrupts disabled to keep it thread-safe. */
    rt_base_t level = rt_hw_interrupt_disable();
    n = lwrb_read(&rx_rb, buf, max_len);
    rt_hw_interrupt_enable(level);

    return (int)n;
}

int rs485_send_then_recv(const uint8_t *tx_buf, uint16_t tx_len,
                         uint8_t *rx_buf, uint16_t max_rx_len,
                         uint32_t timeout_ms)
{
    int ret;

    ret = rs485_send(tx_buf, tx_len, timeout_ms);
    if (ret < 0)
    {
        return ret;
    }

    return rs485_recv(rx_buf, max_rx_len, timeout_ms);
}

/**
 * @brief MSH command: send hex string over RS485 and print the response.
 *
 * Usage:
 *   rs485_send 01 03 00 00 00 0A C5 CD
 *   rs485_send 01030000000AC5CD
 */
static void rs485_send_cmd(int argc, char **argv)
{
    uint8_t tx_buf[RS485_FRAME_SIZE];
    uint8_t rx_buf[RS485_FRAME_SIZE];
    int tx_len;
    int rx_len;
    int i;

    if (argc < 2)
    {
        rt_kprintf("usage: rs485_send <hex bytes>\n");
        rt_kprintf("   eg: rs485_send 01 03 00 00 00 0A C5 CD\n");
        return;
    }

    /* Concatenate all arguments into one string for parsing. */
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

    rt_kprintf("RS485 TX[%d]:", tx_len);
    for (i = 0; i < tx_len; i++)
    {
        rt_kprintf(" %02X", tx_buf[i]);
    }
    rt_kprintf("\n");

    rx_len = rs485_send_then_recv(tx_buf, (uint16_t)tx_len, rx_buf, sizeof(rx_buf), 200);
    if (rx_len < 0)
    {
        rt_kprintf("RS485 send/recv failed: %d\n", rx_len);
        return;
    }

    rt_kprintf("RS485 RX[%d]:", rx_len);
    for (i = 0; i < rx_len; i++)
    {
        rt_kprintf(" %02X", rx_buf[i]);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(rs485_send_cmd, send hex data to RS485 and print response);

/**
 * @brief Auto-initialize RS485 with default baudrate.
 */
static int rs485_auto_init(void)
{
    return rs485_init(115200);
}
INIT_APP_EXPORT(rs485_auto_init);
