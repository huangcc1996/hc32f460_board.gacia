/*
 * Copyright (C) 2022-2024, Xiaohua Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-22     OpenCode     first version
 */

#ifndef __UART_TEST_H__
#define __UART_TEST_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UART test device (uart3) with given baudrate.
 *
 * Change TEST_UART_NAME in uart_test.c to test a different UART port.
 *
 * @param baudrate Baudrate, e.g. 9600, 115200.
 * @return RT_EOK on success, -RT_ERROR on failure.
 */
int uart_test_init(uint32_t baudrate);

/**
 * @brief Send data over UART.
 *
 * @param data  Data buffer to send.
 * @param len   Number of bytes to send.
 * @param timeout_ms Not used currently (kept for API symmetry).
 * @return Number of bytes sent, or -RT_ERROR on failure.
 */
int uart_test_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief Receive data from UART ring buffer.
 *
 * @param buf       Buffer to store received data.
 * @param max_len   Maximum number of bytes to read.
 * @param timeout_ms Timeout in milliseconds.
 * @return Number of bytes actually read, 0 on timeout, -RT_ERROR on failure.
 */
int uart_test_recv(uint8_t *buf, uint16_t max_len, uint32_t timeout_ms);

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
int uart_test_send_then_recv(const uint8_t *tx_buf, uint16_t tx_len,
                             uint8_t *rx_buf, uint16_t max_rx_len,
                             uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __UART_TEST_H__ */
