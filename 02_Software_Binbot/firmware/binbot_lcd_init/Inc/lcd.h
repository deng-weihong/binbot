/**
  ******************************************************************************
  * @file    lcd.h
  * @brief   LCD controller initialization API.
  ******************************************************************************
  */

#ifndef __LCD_H
#define __LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lcd_conf.h"

void LCD_Init(void);
const char *LCD_GetControllerName(void);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_H */
