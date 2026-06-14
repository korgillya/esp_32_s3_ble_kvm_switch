#include "tasks.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_events.h"
#include "app_queues.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_mouse.h"
#include "kvm_state.h"
#include "ssd1306.h"
#include "usb_mouse_host.h"

static const char *TAG_KVM = "task-kvm";
static const char *TAG_UI = "task-ui";
static const char *TAG_BLE = "task-ble";

static kvm_state_t g_state;

static const char *usb_status_text(const kvm_state_t *state)
{
    if (state->usb_mouse_connected) {
        return "MOUSE";
    }
    if (state->usb_hid_seen) {
        return "HID";
    }
    if (state->usb_device_seen) {
        return "DEV";
    }
    if (state->usb_host_started) {
        return "HOST";
    }
    return "WAIT";
}

static const char *device_mode_text(const kvm_state_t *state)
{
    const bool active_ble_connected = state->ble_connected[state->active_host];

    if (ble_mouse_is_pairing()) {
        return "PAIRING";
    }
    if (active_ble_connected && state->usb_mouse_connected) {
        return "WORK";
    }
    if (active_ble_connected) {
        return "CONNECTED";
    }
    if (!state->usb_mouse_connected) {
        return "WAIT USB";
    }
    return "WAIT BLE";
}

static void format_raw_bytes(char *out, size_t out_size, const uint8_t *bytes, uint8_t len)
{
    size_t used = 0;
    const uint8_t count = len > 8 ? 8 : len;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    for (uint8_t i = 0; i < count && used < out_size; i++) {
        const int written = snprintf(out + used, out_size - used, "%s%02X", i == 0 ? "" : " ", bytes[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
}

static void render_status(ssd1306_t *display, const kvm_state_t *state)
{
    char line[32];

    ssd1306_clear(display);
    ssd1306_draw_frame(display, 0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, true);
    ssd1306_draw_text(display, 8, 4, "BLE KVM", true, 1);

    snprintf(line, sizeof(line), "ACTIVE: %s", state->active_host == KVM_HOST_LEFT ? "LEFT" : "RIGHT");
    ssd1306_draw_text(display, 8, 14, line, true, 1);

    snprintf(line, sizeof(line), "USB: %s P%u", usb_status_text(state), state->usb_proto);
    ssd1306_draw_text(display, 8, 24, line, true, 1);

    if (state->usb_device_seen) {
        snprintf(line, sizeof(line), "%04X:%04X", state->usb_vid, state->usb_pid);
    } else {
        snprintf(line, sizeof(line), "NO USB DEVICE");
    }
    ssd1306_draw_text(display, 8, 34, line, true, 1);

    snprintf(line, sizeof(line), "RPT:%" PRIu32 " B:%02X", state->mouse_report_count, state->last_mouse_buttons);
    ssd1306_draw_text(display, 8, 44, line, true, 1);

    snprintf(line, sizeof(line), "MODE: %s", device_mode_text(state));
    ssd1306_draw_text(display, 8, 54, line, true, 1);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int8_t scale_mouse_delta(int16_t delta)
{
    const int32_t scaled = (int32_t)delta * MOUSE_BLE_DELTA_SCALE;
    if (scaled > INT8_MAX) {
        return INT8_MAX;
    }
    if (scaled < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)scaled;
}

static int8_t clamp_mouse_delta(int16_t delta)
{
    if (delta > INT8_MAX) {
        return INT8_MAX;
    }
    if (delta < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)delta;
}

static void post_kvm_event(kvm_event_type_t type, kvm_host_t host)
{
    const kvm_event_t event = {
        .type = type,
        .host = host,
        .tick_ms = now_ms(),
    };
    if (xQueueSend(g_kvm_event_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG_KVM, "KVM event queue full");
    }
}

static void kvm_task(void *arg)
{
    (void)arg;
    kvm_state_init(&g_state);
    ESP_LOGI(TAG_KVM, "KVM state machine started");

    kvm_event_t event;
    while (true) {
        if (xQueueReceive(g_kvm_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            kvm_state_handle_event(&g_state, &event);
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ssd1306_t display;
    uint32_t last_log_ms = 0;
    bool display_ready = ssd1306_init(&display) == ESP_OK;
    if (!display_ready) {
        ESP_LOGW(TAG_UI, "OLED not detected; continuing with serial UI only");
    }

    while (true) {
        const uint32_t current_ms = now_ms();
        if (current_ms - last_log_ms >= 1000) {
            last_log_ms = current_ms;
            ESP_LOGI(TAG_UI,
                     "status active=%s usb=%s device=%04X:%04X proto=%u reports=%" PRIu32
                     " ble=L%d/R%d switches=%" PRIu32 " dropped=%" PRIu32,
                     kvm_host_name(g_state.active_host),
                     usb_status_text(&g_state),
                     g_state.usb_vid,
                     g_state.usb_pid,
                     g_state.usb_proto,
                     g_state.mouse_report_count,
                     g_state.ble_connected[KVM_HOST_LEFT],
                     g_state.ble_connected[KVM_HOST_RIGHT],
                     g_state.switch_count,
                     g_state.dropped_events);
        }
        if (display_ready) {
            render_status(&display, &g_state);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ssd1306_present(&display));
        }
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
    }
}

static void ble_hid_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG_BLE, "BLE HID mouse bridge started");

    ESP_ERROR_CHECK_WITHOUT_ABORT(ble_mouse_start());
    post_kvm_event(KVM_EVENT_BLE_HOST_DISCONNECTED, KVM_HOST_LEFT);
    post_kvm_event(KVM_EVENT_BLE_HOST_DISCONNECTED, KVM_HOST_RIGHT);

    mouse_report_t report;
    mouse_report_t last_logged = {0};
    bool have_last_logged = false;
    uint32_t last_log_ms = 0;
    uint32_t last_send_ms = 0;
    int16_t pending_dx = 0;
    int16_t pending_dy = 0;
    int16_t pending_wheel = 0;
    uint8_t pending_buttons = 0;
    bool have_pending = false;

    while (true) {
        const uint32_t current_ms = now_ms();
        const uint32_t elapsed_ms = current_ms - last_send_ms;
        const uint32_t timeout_ms = have_pending
                                        ? (elapsed_ms >= MOUSE_BLE_REPORT_INTERVAL_MS
                                               ? 0
                                               : MOUSE_BLE_REPORT_INTERVAL_MS - elapsed_ms)
                                        : BLE_STUB_HEARTBEAT_MS;

        if (xQueueReceive(g_mouse_report_queue, &report, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            pending_dx += report.dx;
            pending_dy += report.dy;
            pending_wheel += report.wheel;
            pending_buttons = report.buttons;
            have_pending = true;

            const uint32_t log_ms = now_ms();
            const bool raw_changed = !have_last_logged ||
                                     report.raw_len != last_logged.raw_len ||
                                     memcmp(report.raw, last_logged.raw, sizeof(report.raw)) != 0;
            const bool meaningful = report.dx != 0 || report.dy != 0 || report.wheel != 0 ||
                                    report.buttons != last_logged.buttons;
            const bool button_changed = !have_last_logged || report.buttons != last_logged.buttons;
            const bool wheel_changed = report.wheel != 0;
            const bool log_due = log_ms - last_log_ms >= MOUSE_BLE_LOG_INTERVAL_MS;

            if ((button_changed || wheel_changed || log_due) && (raw_changed || meaningful)) {
                char raw_hex[32];
                format_raw_bytes(raw_hex, sizeof(raw_hex), report.raw, report.raw_len);
                ESP_LOGI(TAG_BLE,
                         "mouse dx=%d dy=%d wheel=%d buttons=0x%02X pending=%d/%d raw_len=%u raw=[%s]",
                         report.dx,
                         report.dy,
                         report.wheel,
                         report.buttons,
                         pending_dx,
                         pending_dy,
                         report.raw_len,
                         raw_hex);
                last_logged = report;
                have_last_logged = true;
                last_log_ms = log_ms;
            }
        }

        const uint32_t send_ms = now_ms();
        if (have_pending && send_ms - last_send_ms >= MOUSE_BLE_REPORT_INTERVAL_MS) {
            if (ble_mouse_is_connected()) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    ble_mouse_send_report(pending_buttons,
                                          scale_mouse_delta(pending_dx),
                                          scale_mouse_delta(pending_dy),
                                          clamp_mouse_delta(pending_wheel)));
            }
            pending_dx = 0;
            pending_dy = 0;
            pending_wheel = 0;
            have_pending = false;
            last_send_ms = send_ms;
        }
    }
}

void kvm_task_start(void)
{
    xTaskCreate(kvm_task, "kvm_task", 4096, NULL, 6, NULL);
}

void ui_task_start(void)
{
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 2, NULL);
}

void usb_host_task_start(void)
{
    ESP_ERROR_CHECK(usb_mouse_host_start());
}

void ble_hid_task_start(void)
{
    xTaskCreate(ble_hid_task, "ble_hid_task", 4096, NULL, 4, NULL);
}
