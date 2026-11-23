/**
  ******************************************************************************
  * @file    lcd_conf.h
  * @brief   LCD controller and 3-wire SPI pin configuration.
  ******************************************************************************
  */

#ifndef __LCD_CONF_H
#define __LCD_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "py32f0xx_hal.h"

#define LCD_CONTROLLER_ST7701       1U
#define LCD_CONTROLLER_NV3052CGRB   2U

#ifndef LCD_CONTROLLER
#define LCD_CONTROLLER              LCD_CONTROLLER_NV3052CGRB
#endif

#if (LCD_CONTROLLER != LCD_CONTROLLER_ST7701) && \
    (LCD_CONTROLLER != LCD_CONTROLLER_NV3052CGRB)
#error "Unsupported LCD_CONTROLLER selection"
#endif

/* 3-wire SPI pins for LCD register initialization. */
#ifndef LCD_SPI_CS_GPIO_PORT
#define LCD_SPI_CS_GPIO_PORT        GPIOA
#endif

#ifndef LCD_SPI_CS_PIN
#define LCD_SPI_CS_PIN              GPIO_PIN_6
#endif

#ifndef LCD_SPI_SCL_GPIO_PORT
#define LCD_SPI_SCL_GPIO_PORT       GPIOB
#endif

#ifndef LCD_SPI_SCL_PIN
#define LCD_SPI_SCL_PIN             GPIO_PIN_0
#endif

#ifndef LCD_SPI_SDA_GPIO_PORT
#define LCD_SPI_SDA_GPIO_PORT       GPIOA
#endif

#ifndef LCD_SPI_SDA_PIN
#define LCD_SPI_SDA_PIN             GPIO_PIN_0
#endif

#ifndef LCD_GPIO_CLK_ENABLE
#define LCD_GPIO_CLK_ENABLE()       do { __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); } while (0)
#endif

/*
 * This board currently has no LCD reset pin in the existing code.
 * Set LCD_USE_RESET_PIN to 1U and define LCD_RST_* macros if the schematic has one.
 */
#ifndef LCD_USE_RESET_PIN
#define LCD_USE_RESET_PIN           0U
#endif

#if (LCD_USE_RESET_PIN != 0U)
#ifndef LCD_RST_GPIO_PORT
#define LCD_RST_GPIO_PORT           GPIOA
#endif

#ifndef LCD_RST_PIN
#define LCD_RST_PIN                 GPIO_PIN_1
#endif

#ifndef LCD_RST_ACTIVE_LEVEL
#define LCD_RST_ACTIVE_LEVEL        GPIO_PIN_RESET
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* __LCD_CONF_H */
