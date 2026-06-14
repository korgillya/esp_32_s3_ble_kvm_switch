#include "board_io.h"

#include "app_config.h"
#include "app_queues.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_state.h"

static const char *TAG = "board";

#define BUTTON_DEBOUNCE_SAMPLES 3
#define BUTTON_PRESSED_LEVEL 1

typedef struct {
    gpio_num_t pin;
    kvm_host_t host;
    kvm_event_type_t switch_event;
    kvm_event_type_t pair_event;
    bool stable_pressed;
    bool last_raw;
    uint8_t streak;
    bool armed;
    uint32_t pressed_at_ms;
} button_state_t;

static void post_button_event(kvm_event_type_t type, kvm_host_t host)
{
    const kvm_event_t event = {
        .type = type,
        .host = host,
        .tick_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    if (xQueueSend(g_kvm_event_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "KVM event queue full; dropped button event");
    }
}

static void button_scan_task(void *arg)
{
    (void)arg;
    button_state_t buttons[] = {
        {
            .pin = PIN_BUTTON_LEFT,
            .host = KVM_HOST_LEFT,
            .switch_event = KVM_EVENT_BUTTON_LEFT,
            .pair_event = KVM_EVENT_PAIR_LEFT,
        },
        {
            .pin = PIN_BUTTON_RIGHT,
            .host = KVM_HOST_RIGHT,
            .switch_event = KVM_EVENT_BUTTON_RIGHT,
            .pair_event = KVM_EVENT_PAIR_RIGHT,
        },
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        button_state_t *button = &buttons[i];
        const bool raw = gpio_get_level(button->pin) == BUTTON_PRESSED_LEVEL;
        button->stable_pressed = raw;
        button->last_raw = raw;
        button->streak = BUTTON_DEBOUNCE_SAMPLES;
        button->armed = !raw;
        button->pressed_at_ms = 0;
        if (raw) {
            ESP_LOGW(TAG,
                     "button %s is active at boot; ignoring until released",
                     kvm_host_name(button->host));
        }
    }

    while (true) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            button_state_t *button = &buttons[i];
            const bool raw = gpio_get_level(button->pin) == BUTTON_PRESSED_LEVEL;

            if (raw == button->last_raw) {
                if (button->streak < UINT8_MAX) {
                    button->streak++;
                }
            } else {
                button->streak = 1;
            }
            button->last_raw = raw;

            if (button->streak >= BUTTON_DEBOUNCE_SAMPLES &&
                raw != button->stable_pressed) {
                button->stable_pressed = raw;
                if (raw) {
                    button->pressed_at_ms = now_ms;
                    ESP_LOGI(TAG, "button %s: press",
                             kvm_host_name(button->host));
                } else {
                    const uint32_t duration = now_ms - button->pressed_at_ms;
                    ESP_LOGI(TAG,
                             "button %s: release after %" PRIu32 " ms (armed=%d)",
                             kvm_host_name(button->host),
                             duration,
                             button->armed);
                    if (!button->armed) {
                        button->armed = true;
                        continue;
                    }
                    if (duration >= BUTTON_LONG_PRESS_MS && duration <= BUTTON_STUCK_PRESS_MS) {
                        ESP_LOGI(TAG, "long press on %s button: pairing mode",
                                 kvm_host_name(button->host));
                        board_beep_pair();
                        post_button_event(button->pair_event, button->host);
                    } else if (duration < BUTTON_LONG_PRESS_MS) {
                        post_button_event(button->switch_event, button->host);
                    } else {
                        ESP_LOGW(TAG,
                                 "button %s: ignored stuck press after %" PRIu32 " ms",
                                 kvm_host_name(button->host),
                                 duration);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_MS));
    }
}

esp_err_t board_io_init(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << PIN_LED_LEFT) | (1ULL << PIN_LED_RIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_LEFT) | (1ULL << PIN_BUTTON_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 2200,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    xTaskCreate(button_scan_task, "button_scan", 3072, NULL, 3, NULL);

    board_set_active_host_leds(KVM_HOST_LEFT);
    ESP_LOGI(TAG, "GPIO initialized: buttons=%d/%d leds=%d/%d buzzer=%d",
             PIN_BUTTON_LEFT, PIN_BUTTON_RIGHT, PIN_LED_LEFT, PIN_LED_RIGHT, PIN_BUZZER);
    return ESP_OK;
}

esp_err_t board_i2c_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &config));
    esp_err_t result = i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0);
    if (result == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(result);
    ESP_LOGI(TAG, "I2C initialized: sda=%d scl=%d freq=%d", PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    return ESP_OK;
}

void board_set_active_host_leds(kvm_host_t host)
{
    gpio_set_level(PIN_LED_LEFT, host == KVM_HOST_LEFT);
    gpio_set_level(PIN_LED_RIGHT, host == KVM_HOST_RIGHT);
}

void board_set_usb_status(bool connected)
{
    ESP_LOGI(TAG, "USB mouse status: %s", connected ? "connected" : "disconnected");
}

void board_beep_switch(kvm_host_t host)
{
    const uint32_t freq_hz = host == KVM_HOST_LEFT ? 1700 : 2600;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    vTaskDelay(pdMS_TO_TICKS(55));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

void board_beep_pair(void)
{
    for (int i = 0; i < 2; i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, 3200));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        vTaskDelay(pdMS_TO_TICKS(70));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
