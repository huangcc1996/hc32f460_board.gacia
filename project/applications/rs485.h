/*
 * Copyright (C) 2022-2024, Xiaohua Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-22     OpenCode     first version
 */

#ifndef __RS485_H__
#define __RS485_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize RS485 device (UART3) and direction control pin.
 *
 * @param baudrate Baudrate, e.g. 9600, 115200.
 * @return RT_EOK on success, -RT_ERROR on failure.
 */
int rs485_init(uint32_t baudrate);

/**
 * @brief Send data over RS485.
 *
 * The function switches the transceiver to TX mode, writes the data,
 * waits until the UART shift register is empty, and then switches back
 * to RX mode.
 *
 * @param data  Data buffer to send.
 * @param len   Number of bytes to send.
 * @param timeout_ms Not used currently (kept for API symmetry).
 * @return Number of bytes sent, or -RT_ERROR on failure.
 */
int rs485_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief Receive data from RS485 ring buffer.
 *
 * @param buf       Buffer to store received data.
 * @param max_len   Maximum number of bytes to read.
 * @param timeout_ms Timeout in milliseconds.
 * @return Number of bytes actually read, 0 on timeout, -RT_ERROR on failure.
 */
int rs485_recv(uint8_t *buf, uint16_t max_len, uint32_t timeout_ms);

/**
 * @brief Send data and wait for response.
 *
 * This is a convenience helper for request/response style debugging.
 *
 * @param tx_buf       Data to send.
 * @param tx_len       Length of tx data.
 * @param rx_buf       Buffer for response.
 * @param max_rx_len   Maximum response length.
 * @param timeout_ms   Timeout waiting for response.
 * @return Number of bytes received, 0 on timeout, -RT_ERROR on failure.
 */
int rs485_send_then_recv(const uint8_t *tx_buf, uint16_t tx_len,
                         uint8_t *rx_buf, uint16_t max_rx_len,
                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __RS485_H__ */
