# BinBot LCD 初始化固件

固件运行在普冉 PY32F002Bx5（Arm Cortex-M0+）上。MCU 上电后通过三线 SPI 发送 9 位命令和数据，初始化 ST7701 或 NV3052CGRB LCD 控制器。命令表和发送逻辑见 [Src/lcd.c](Src/lcd.c)。

## 屏幕选择与引脚

工程默认使用 NV3052CGRB。使用 ST7701 时，在 Keil 的 C/C++ 预处理宏中添加 `LCD_CONTROLLER=LCD_CONTROLLER_ST7701`。屏幕选择和引脚定义见 [Inc/lcd_conf.h](Inc/lcd_conf.h)。

| LCD 信号 | PY32F002B 引脚 |
| --- | --- |
| CS | PA6 |
| SCL | PB0 |
| SDA | PA0 |

默认禁用独立复位脚。板上接有复位脚时，使用 `LCD_USE_RESET_PIN=1`，并按原理图设置 `LCD_RST_GPIO_PORT`、`LCD_RST_PIN` 和 `LCD_RST_ACTIVE_LEVEL`。

## 构建

1. 在 Windows 上安装 Keil MDK-ARM 和 Arm Compiler 6.22。从 Pack Installer 安装 Puya PY32F0xx DFP 1.2.6 和 ARM CMSIS Pack（工程引用 5.8.0）。
2. 在 µVision 中打开 [MDK-ARM/binbot_lcd_init.uvprojx](MDK-ARM/binbot_lcd_init.uvprojx)，选择 `binbot_lcd_init` 目标后执行 Build。
3. 烧录镜像位于 `MDK-ARM/Output/binbot_lcd_init.hex`，符号文件位于同目录的 `binbot_lcd_init.axf`。

## 第三方代码

`Drivers/` 收录工程用到的 Puya HAL、器件头文件和 Arm CMSIS。Puya 与 STMicroelectronics 代码的 BSD 3-Clause 声明及 CMSIS 的许可保留在原文件头。 [BSD 3-Clause](https://opensource.org/license/bsd-3-clause)
