#include "board.h"
#include "board_io.h"

#include "driver/gpio.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdatomic.h>

static const char *TAG = "board";

static _Atomic indicator_pattern_t s_pattern = IND_OFF;
static _Atomic button_event_t      s_button_event = BTN_NONE;

static led_strip_handle_t s_strip;

/* ------- low-level helpers ------- */

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void board_led_set(bool green_on, bool red_on)
{
    if (green_on && red_on)  rgb_set(32, 24,  0);   /* amber  */
    else if (green_on)       rgb_set( 0, 32,  0);   /* green  */
    else if (red_on)         rgb_set(32,  0,  0);   /* red    */
    else                     rgb_set( 0,  0,  0);   /* off    */
}

void board_buzzer_set(bool on)
{
    gpio_set_level(BOARD_PIN_BUZZER, on ? 1 : 0);
}

void board_indicate(indicator_pattern_t p)
{
    atomic_store(&s_pattern, p);
}

/* ------- indicator task -------
 *
 * One source of truth for LED + buzzer state. The task runs the pattern
 * matching s_pattern; if the pattern changes mid-sequence, it's picked up at
 * the next step boundary. Patterns that are "events" (GRANT, DENY, WROTE_OK)
 * play once and then revert to whichever ARMED_* mode is current.
 */

static indicator_pattern_t s_resting = IND_IDLE;

static void play_grant(void)
{
    rgb_set(0, 64, 0);            /* bright green */
    board_buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(120));
    board_buzzer_set(false);
    vTaskDelay(pdMS_TO_TICKS(700));
    rgb_set(0, 0, 0);
}

static void play_deny(void)
{
    for (int i = 0; i < 2; i++) {
        rgb_set(64, 0, 0);        /* red */
        board_buzzer_set(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        rgb_set(0, 0, 0);
        board_buzzer_set(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void play_wrote_ok(void)
{
    for (int i = 0; i < 3; i++) {
        rgb_set(0, 32, 32);       /* cyan */
        board_buzzer_set(true);
        vTaskDelay(pdMS_TO_TICKS(80));
        rgb_set(0, 0, 0);
        board_buzzer_set(false);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void play_error(void)
{
    board_buzzer_set(true);
    for (int i = 0; i < 6; i++) {
        rgb_set(64, 0, 0);        /* fast red flash */
        vTaskDelay(pdMS_TO_TICKS(80));
        rgb_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    board_buzzer_set(false);
}

static void indicator_task(void *arg)
{
    bool blink = false;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        indicator_pattern_t p = atomic_load(&s_pattern);

        switch (p) {
        case IND_OFF:
            rgb_set(0, 0, 0);
            board_buzzer_set(false);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
            break;

        case IND_IDLE:
            s_resting = IND_IDLE;
            blink = !blink;
            rgb_set(0, 0, blink ? 8 : 0);  /* dim blue heartbeat */
            board_buzzer_set(false);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(1500));
            break;

        case IND_ARMED_READ:
            s_resting = IND_ARMED_READ;
            rgb_set(0, 32, 0);              /* solid green */
            board_buzzer_set(false);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(200));
            break;

        case IND_ARMED_WRITE:
            s_resting = IND_ARMED_WRITE;
            blink = !blink;
            rgb_set(blink ? 32 : 0, blink ? 16 : 0, 0);  /* slow amber blink */
            board_buzzer_set(false);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(500));
            break;

        case IND_GRANT:
            play_grant();
            atomic_store(&s_pattern, s_resting);
            break;

        case IND_DENY:
            play_deny();
            atomic_store(&s_pattern, s_resting);
            break;

        case IND_WROTE_OK:
            play_wrote_ok();
            atomic_store(&s_pattern, s_resting);
            break;

        case IND_ERROR:
            play_error();
            atomic_store(&s_pattern, s_resting);
            break;
        }
    }
}

/* ------- button (polled with debounce + long-press detection) ------- */

static void button_task(void *arg)
{
    bool prev = true;                    /* idle = high (pull-up) */
    int64_t press_us = 0;

    for (;;) {
        bool now = gpio_get_level(BOARD_PIN_BUTTON);

        if (prev && !now) {              /* falling edge — pressed */
            press_us = esp_timer_get_time();
        } else if (!prev && now && press_us) {   /* rising edge — released */
            int64_t held_ms = (esp_timer_get_time() - press_us) / 1000;
            press_us = 0;

            if (held_ms >= 50 && held_ms < 1000) {
                atomic_store(&s_button_event, BTN_SHORT);
            } else if (held_ms >= 2000 && held_ms < 10000) {
                atomic_store(&s_button_event, BTN_LONG);
            } else if (held_ms >= 10000) {
                atomic_store(&s_button_event, BTN_FACTORY_RESET);
            }
        }

        prev = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

button_event_t board_button_poll(void)
{
    button_event_t e = atomic_exchange(&s_button_event, BTN_NONE);
    return e;
}

/* ------- init ------- */

void board_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = BOARD_PIN_RGB_LED,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BUZZER),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    board_buzzer_set(false);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BUTTON),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    xTaskCreatePinnedToCore(indicator_task, "ind",   2048, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(button_task,    "btn",   2048, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "board init complete");
}
