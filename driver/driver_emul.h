/***************************************************************************//**
 * @file
 * @brief Co-Processor Communication Protocol(CPC) - Emulation driver
 * @version 3.2.0
 *******************************************************************************
 * # License
 * <b>Copyright 2021 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#ifndef DRV_UART_H
#define DRV_UART_H

#include <stdbool.h>
#include <pthread.h>
#include "sl_status.h"
#include "core.h"

/*
 * Initialize the uart driver. Crashes the app if the init fails.
 * Returns the file descriptor of the paired socket to the driver
 * to use in a select() call.
 */
pthread_t driver_emul_init(int* fd_core_driver);
sl_status_t sli_cpc_drv_read_data(frame_t *handle, uint16_t *payload_rx_len);
void sli_cpc_drv_emul_submit_pkt_for_rx(void *header_buf, void *payload_buf, uint16_t payload_buf_len);
void sli_cpc_drv_emul_set_ep_state(uint8_t id, cpc_endpoint_state_t state);

#endif