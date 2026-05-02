#pragma once

/*
 * Pin map — ESP32-S3-WROOM-1 (44-pin dev board)
 *
 * Avoid: GPIO19/20 (USB D-/D+), GPIO26-32 (octal flash),
 *        GPIO33-37 (PSRAM), GPIO43/44 (UART0 USB bridge),
 *        GPIO0 (BOOT button / strapping), GPIO3/45/46 (strapping)
 *
 * PN5180 NFC (SPI2):
 *   SCK  -> GPIO 12   MISO -> GPIO 13   MOSI -> GPIO 11
 *   NSS  -> GPIO 10   BUSY -> GPIO 14   RST  -> GPIO 15
 *   IRQ  -> GPIO 16
 *   VCC  -> 5V (RF TX driver)  — add 10µF + 100nF at TVDD pin
 *
 * RS485 / OSDP (UART1, HiLetgo auto-flow board — no DE/RE pin needed):
 *   RXD (board input)  -> GPIO 17   (UART1 TX from ESP)
 *   TXD (board output) -> GPIO 18   (UART1 RX into ESP)
 *
 * Indicators (on ESP32-S3 dev board — no external hardware needed):
 *   WS2812B RGB LED -> GPIO 48
 *   Active piezo    -> GPIO 21   (HIGH = on)
 *   BOOT button     -> GPIO  0   (active LOW, internal pull-up)
 */

/* PN5180 SPI */
#define BOARD_PN5180_SPI_HOST    SPI2_HOST
#define BOARD_PN5180_PIN_SCK     12
#define BOARD_PN5180_PIN_MISO    13
#define BOARD_PN5180_PIN_MOSI    11
#define BOARD_PN5180_PIN_NSS     10
#define BOARD_PN5180_PIN_BUSY    14
#define BOARD_PN5180_PIN_RST     15
#define BOARD_PN5180_PIN_IRQ     16
#define BOARD_PN5180_SPI_HZ     (7 * 1000 * 1000)

/* RS485 / OSDP */
#define BOARD_OSDP_UART_NUM      UART_NUM_1
#define BOARD_OSDP_PIN_TX        17
#define BOARD_OSDP_PIN_RX        18
#define BOARD_OSDP_BAUD          9600

/* Indicators */
#define BOARD_PIN_RGB_LED        48
#define BOARD_PIN_BUZZER         21
#define BOARD_PIN_BUTTON          0

/* OSDP default identity (Phase 2) */
#define BOARD_OSDP_PD_ADDRESS    0x65
