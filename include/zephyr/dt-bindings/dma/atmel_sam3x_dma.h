/*
 * Copyright (c) 2020 Linaro Limited
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_ATMEL_SAM3X_DMA_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_ATMEL_SAM3X_DMA_H_

/**
 * Atmel SAM3x Peripheral Hardware Request HW Interface Number (XDMAC_CC.PERID).
 *
 * See table Table 22-2. DMA Controller in the SAM3X / SAM3A datashee.
 */
#define DMA_PERID_HSMCI_TX_RX 0
#define DMA_PERID_SPI0_TX     1
#define DMA_PERID_SPI0_RX     2
#define DMA_PERID_SSC_TX      3
#define DMA_PERID_SSC_RX      4
#define DMA_PERID_SPI1_TX     5
#define DMA_PERID_SPI1_RX     6
#define DMA_PERID_TWIHS0_TX   7
#define DMA_PERID_TWIHS0_RX   8
#define DMA_PERID_USART0_TX   11
#define DMA_PERID_USART0_RX   12
#define DMA_PERID_USART1_TX   13
#define DMA_PERID_USART1_RX   14
#define DMA_PERID_PWM0_TX     15

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_ATMEL_SAM3X_DMA_H_ */
