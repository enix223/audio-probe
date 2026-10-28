#ifndef _PROJ_CONFIG_H_
#define _PROJ_CONFIG_H_

#define DISABLE (0)
#define ENABLE (1)

#define __CONFIG_OS_KERNEL

/*
 * Clock settings
 */
#define XTAL_CLOCK (40000000)
#define RCO_CLOCK (32000)
#define PLL_CLOCK (160000000)
#define SYSTEM_CLOCK (160000000)

/*
 * Module enable/disable control
 */
#define FLASH_XIP ENABLE
#define LN_ASSERT_EN ENABLE
#define HAL_ASSERT_EN ENABLE
#define PRINTF_OMIT DISABLE
#define OS_TICK_COMPENSATE

/* Enable NTP support */
#define CFG_USE_NTP ENABLE

/* SNTP server DNS resolution */
#define SNTP_SERVER_DNS 1

/* BLE support */
#define BLE_DATA_TRANS_SERVER ENABLE

/* Disable AT command (not needed for iot-client) */
#define LN_AT_CMD_SUPPORT DISABLE

/* Disable iperf (not needed) */
#define IPERF_SUPPORT DISABLE

/* Disable LN_AT (not needed) */
#define LN_AT_SUPPORT DISABLE

// Check endianness
#if defined(__CC_ARM)
#if defined(__BIG_ENDIAN)
#error "Please set the compiler to little-endian mode"
#endif
#elif defined(__GNUC__)
#if (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "Please set the compiler to little-endian mode"
#endif
#else
#error "Unsupported compiler"
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif

/*
 * flash image settings
 */
#define FLASH_IMAGE_VER_MAJOR 0
#define FLASH_IMAGE_VER_MINOR 1
#define SOC_CRP_FLAG 0

/*
 * Hardware config
 */
#define CFG_UART0_TX_BUF_SIZE 256
#define CFG_UART0_RX_BUF_SIZE 256
#define CFG_UART1_TX_BUF_SIZE 128
#define CFG_UART1_RX_BUF_SIZE 2048
#define CFG_UART2_TX_BUF_SIZE 128
#define CFG_UART2_RX_BUF_SIZE 256

#define CFG_UART_BAUDRATE_LOG 921600
#define CFG_UART_BAUDRATE_CONSOLE 115200

#endif /* _PROJ_CONFIG_H_ */
