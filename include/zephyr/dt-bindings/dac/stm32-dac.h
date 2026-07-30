/*
 * Copyright (c) 2026 Draeger
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief STM32 DAC devicetree binding constants.
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_DAC_STM32_DAC_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_DAC_STM32_DAC_H_

/**
 * @name STM32 DAC trigger sources
 *
 * Values for the `trigger` devicetree property.
 * @{
 */
/** Trigger DAC conversions from the TIM6 TRGO output. */
#define STM32_DAC_TRIG_TIM6 6

/** Trigger DAC conversions from the TIM7 TRGO output. */
#define STM32_DAC_TRIG_TIM7 7
/** @} */

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_DAC_STM32_DAC_H_ */
