#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Board I/O — LED, buzzer, and button.
 *
 * indicator_pattern_t drives an internal task so callers never block.
 * Event patterns (GRANT, DENY) play once, then the task reverts to the
 * last resting pattern (IDLE or ARMED_READ).
 */

typedef enum {
    IND_OFF = 0,
    IND_IDLE,           /* dim blue heartbeat */
    IND_ARMED_READ,     /* solid green — scanning for cards */
    IND_GRANT,          /* green flash + short beep — card OK */
    IND_DENY,           /* red flash + double beep — card error */
    IND_ERROR,          /* fast red + long beep — NFC failure */
} indicator_pattern_t;

typedef enum {
    BTN_NONE = 0,
    BTN_SHORT,          /* < 1s press */
    BTN_LONG,           /* >= 2s press */
    BTN_FACTORY_RESET,  /* >= 10s press */
} button_event_t;

void board_init(void);

/* Direct control — used when libosdp issues LED/buzzer commands (Phase 2) */
void board_led_set(bool green_on, bool red_on);
void board_buzzer_set(bool on);

/* Async pattern — returns immediately; indicator task drives the hardware */
void board_indicate(indicator_pattern_t p);

/* Non-blocking — returns and clears the latched edge event */
button_event_t board_button_poll(void);
