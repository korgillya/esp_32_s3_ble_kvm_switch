#include "usb_mouse_host.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "app_events.h"
#include "app_queues.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb-mouse";

static void post_usb_device_event(kvm_event_type_t type, uint16_t vid, uint16_t pid, uint8_t proto);

typedef struct {
    bool valid;
    uint8_t report_id;
    bool has_report_id;
    int x_bit;
    int x_size;
    int y_bit;
    int y_size;
    int wheel_bit;
    int wheel_size;
    int buttons_bit;
    int buttons_count;
} mouse_report_format_t;

static mouse_report_format_t s_report_format;
static uint16_t s_device_vid;
static uint16_t s_device_pid;

#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
static bool usb_enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *b_configuration_value)
{
    *b_configuration_value = 1;
    ESP_LOGI(TAG,
             "Enumerating USB device: VID=0x%04X PID=0x%04X class=0x%02X configs=%u",
             dev_desc->idVendor,
             dev_desc->idProduct,
             dev_desc->bDeviceClass,
             dev_desc->bNumConfigurations);
    post_usb_device_event(KVM_EVENT_USB_DEVICE_SEEN, dev_desc->idVendor, dev_desc->idProduct, 0);
    return true;
}
#endif

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void post_kvm_event(kvm_event_type_t type)
{
    const kvm_event_t event = {
        .type = type,
        .host = KVM_HOST_LEFT,
        .tick_ms = now_ms(),
    };
    if (g_kvm_event_queue != NULL) {
        xQueueSend(g_kvm_event_queue, &event, pdMS_TO_TICKS(20));
    }
}

static void post_usb_device_event(kvm_event_type_t type, uint16_t vid, uint16_t pid, uint8_t proto)
{
    const kvm_event_t event = {
        .type = type,
        .host = KVM_HOST_LEFT,
        .tick_ms = now_ms(),
        .usb_vid = vid,
        .usb_pid = pid,
        .usb_proto = proto,
    };
    if (g_kvm_event_queue != NULL) {
        xQueueSend(g_kvm_event_queue, &event, pdMS_TO_TICKS(20));
    }
}

static void post_mouse_report_event(const mouse_report_t *report)
{
    kvm_event_t event = {
        .type = KVM_EVENT_USB_MOUSE_REPORT,
        .host = KVM_HOST_LEFT,
        .tick_ms = now_ms(),
        .mouse_dx = report->dx,
        .mouse_dy = report->dy,
        .mouse_wheel = report->wheel,
        .mouse_buttons = report->buttons,
        .mouse_raw_len = report->raw_len,
    };
    memcpy(event.mouse_raw, report->raw, sizeof(event.mouse_raw));
    if (g_kvm_event_queue != NULL) {
        xQueueSend(g_kvm_event_queue, &event, 0);
    }
}

static int32_t sign_extend(uint32_t value, int bits)
{
    if (bits <= 0 || bits >= 32) {
        return (int32_t)value;
    }
    const uint32_t sign_bit = 1U << (bits - 1);
    if ((value & sign_bit) == 0) {
        return (int32_t)value;
    }
    return (int32_t)(value | (~0U << bits));
}

static uint32_t get_bits_le(const uint8_t *data, size_t len, int bit_offset, int bit_size, bool has_report_id)
{
    uint32_t value = 0;
    const int byte_offset = has_report_id ? 1 : 0;

    for (int i = 0; i < bit_size; i++) {
        const int source_bit = bit_offset + i;
        const int source_byte = byte_offset + (source_bit / 8);
        if (source_byte >= (int)len) {
            break;
        }
        if ((data[source_byte] & (1U << (source_bit % 8))) != 0) {
            value |= 1U << i;
        }
    }
    return value;
}

static bool parse_descriptor_mouse_report(const uint8_t *data, size_t len, mouse_report_t *out_report)
{
    const mouse_report_format_t *fmt = &s_report_format;
    if (!fmt->valid) {
        return false;
    }
    if (fmt->has_report_id && (len == 0 || data[0] != fmt->report_id)) {
        return false;
    }

    out_report->buttons = 0;
    if (fmt->buttons_bit >= 0 && fmt->buttons_count > 0) {
        const int buttons_to_read = fmt->buttons_count > 8 ? 8 : fmt->buttons_count;
        out_report->buttons = (uint8_t)get_bits_le(data, len, fmt->buttons_bit, buttons_to_read, fmt->has_report_id);
    }

    if (fmt->x_bit >= 0 && fmt->x_size > 0) {
        out_report->dx = (int8_t)sign_extend(get_bits_le(data, len, fmt->x_bit, fmt->x_size, fmt->has_report_id), fmt->x_size);
    }
    if (fmt->y_bit >= 0 && fmt->y_size > 0) {
        out_report->dy = (int8_t)sign_extend(get_bits_le(data, len, fmt->y_bit, fmt->y_size, fmt->has_report_id), fmt->y_size);
    }
    if (fmt->wheel_bit >= 0 && fmt->wheel_size > 0) {
        out_report->wheel = (int8_t)sign_extend(get_bits_le(data, len, fmt->wheel_bit, fmt->wheel_size, fmt->has_report_id), fmt->wheel_size);
    }
    return true;
}

static int16_t read_i16_le(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int8_t clamp_i16_to_i8(int16_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static bool is_logitech_g305_receiver(void)
{
    return s_device_vid == 0x046D && s_device_pid == 0xC53F;
}

static bool parse_logitech_g305_report(const uint8_t *data, size_t len, mouse_report_t *out_report)
{
    if (!is_logitech_g305_receiver()) {
        return false;
    }

    if (len < 8 || data[0] != 0x02) {
        return false;
    }

    out_report->buttons = data[1] & 0x1F;
    out_report->dx = clamp_i16_to_i8(read_i16_le(&data[3]));
    out_report->dy = clamp_i16_to_i8(read_i16_le(&data[5]));
    out_report->wheel = (int8_t)data[7];
    return true;
}

static void format_raw_bytes(char *out, size_t out_size, const uint8_t *data, size_t len)
{
    size_t used = 0;
    const size_t count = len > 8 ? 8 : len;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < count && used < out_size; i++) {
        const int written = snprintf(out + used, out_size - used, "%s%02X", i == 0 ? "" : " ", data[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
}

static void log_unparsed_g305_report(const uint8_t *data, size_t len)
{
    static uint8_t last_raw[8];
    static size_t last_len;
    static uint32_t log_count;

    const size_t copy_len = len > sizeof(last_raw) ? sizeof(last_raw) : len;
    if (last_len == len && memcmp(last_raw, data, copy_len) == 0 && log_count > 20) {
        return;
    }

    char raw_hex[32];
    format_raw_bytes(raw_hex, sizeof(raw_hex), data, len);
    ESP_LOGW(TAG, "G305 unparsed report raw_len=%u raw=[%s]", (unsigned)len, raw_hex);

    memset(last_raw, 0, sizeof(last_raw));
    memcpy(last_raw, data, copy_len);
    last_len = len;
    log_count++;
}

static bool parse_boot_mouse_report(const uint8_t *data, size_t len, mouse_report_t *out_report)
{
    if (len < 3) {
        return false;
    }

    size_t offset = (len >= 4 && data[0] == 0x02) ? 1 : 0;
    if (len < offset + 3) {
        return false;
    }

    out_report->buttons = data[offset] & 0x1F;
    out_report->dx = (int8_t)data[offset + 1];
    out_report->dy = (int8_t)data[offset + 2];
    out_report->wheel = 0;
    if (len > offset + 3) {
        out_report->wheel = (int8_t)data[offset + 3];
    }
    return true;
}

static int32_t hid_item_value(const uint8_t *bytes, size_t size, bool is_signed)
{
    uint32_t value = 0;
    for (size_t i = 0; i < size; i++) {
        value |= (uint32_t)bytes[i] << (8 * i);
    }
    if (!is_signed || size == 0 || size >= 4) {
        return (int32_t)value;
    }
    return sign_extend(value, (int)size * 8);
}

static void clear_local_usages(uint16_t *usages, size_t *usage_count, bool *has_usage_range, uint16_t *usage_min, uint16_t *usage_max)
{
    *usage_count = 0;
    *has_usage_range = false;
    *usage_min = 0;
    *usage_max = 0;
    memset(usages, 0, sizeof(uint16_t) * 16);
}

static void parse_mouse_report_descriptor(const uint8_t *desc, size_t len)
{
    mouse_report_format_t fmt = {
        .valid = false,
        .report_id = 0,
        .has_report_id = false,
        .x_bit = -1,
        .x_size = 0,
        .y_bit = -1,
        .y_size = 0,
        .wheel_bit = -1,
        .wheel_size = 0,
        .buttons_bit = -1,
        .buttons_count = 0,
    };
    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t report_id = 0;
    uint16_t usages[16] = {0};
    size_t usage_count = 0;
    bool has_usage_range = false;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    uint16_t bit_offsets[256] = {0};

    for (size_t pos = 0; pos < len;) {
        const uint8_t prefix = desc[pos++];
        if (prefix == 0xFE) {
            if (pos + 1 >= len) {
                break;
            }
            const uint8_t long_size = desc[pos++];
            pos++;
            pos += long_size;
            continue;
        }

        size_t item_size = prefix & 0x03;
        if (item_size == 3) {
            item_size = 4;
        }
        if (pos + item_size > len) {
            break;
        }

        const uint8_t item_type = (prefix >> 2) & 0x03;
        const uint8_t item_tag = (prefix >> 4) & 0x0F;
        const uint8_t *item_data = &desc[pos];
        const uint32_t unsigned_value = (uint32_t)hid_item_value(item_data, item_size, false);
        const int32_t signed_value = hid_item_value(item_data, item_size, true);
        pos += item_size;

        if (item_type == 1) {
            switch (item_tag) {
            case 0x0:
                usage_page = (uint16_t)unsigned_value;
                break;
            case 0x1:
                logical_min = signed_value;
                break;
            case 0x7:
                report_size = unsigned_value;
                break;
            case 0x8:
                report_id = (uint8_t)unsigned_value;
                fmt.has_report_id = true;
                break;
            case 0x9:
                report_count = unsigned_value;
                break;
            default:
                break;
            }
            continue;
        }

        if (item_type == 2) {
            switch (item_tag) {
            case 0x0:
                if (usage_count < 16) {
                    usages[usage_count++] = (uint16_t)unsigned_value;
                }
                break;
            case 0x1:
                usage_min = (uint16_t)unsigned_value;
                has_usage_range = true;
                break;
            case 0x2:
                usage_max = (uint16_t)unsigned_value;
                has_usage_range = true;
                break;
            default:
                break;
            }
            continue;
        }

        if (item_type == 0 && item_tag == 0x8) {
            const bool is_constant = (unsigned_value & 0x01) != 0;
            const int base_bit = bit_offsets[report_id];

            if (!is_constant && report_size > 0 && report_count > 0) {
                for (uint32_t i = 0; i < report_count; i++) {
                    uint16_t usage = 0;
                    if (i < usage_count) {
                        usage = usages[i];
                    } else if (has_usage_range && usage_min <= usage_max) {
                        usage = usage_min + i;
                    } else if (usage_count > 0) {
                        usage = usages[usage_count - 1];
                    }

                    const int field_bit = base_bit + (int)(i * report_size);
                    if (usage_page == 0x01 && usage == 0x30) {
                        fmt.report_id = report_id;
                        fmt.x_bit = field_bit;
                        fmt.x_size = (int)report_size;
                    } else if (usage_page == 0x01 && usage == 0x31) {
                        fmt.report_id = report_id;
                        fmt.y_bit = field_bit;
                        fmt.y_size = (int)report_size;
                    } else if (usage_page == 0x01 && usage == 0x38) {
                        fmt.wheel_bit = field_bit;
                        fmt.wheel_size = (int)report_size;
                    } else if (usage_page == 0x09 && fmt.buttons_bit < 0) {
                        fmt.report_id = report_id;
                        fmt.buttons_bit = field_bit;
                        fmt.buttons_count = (int)report_count;
                    }
                }
            }

            bit_offsets[report_id] += (uint16_t)(report_size * report_count);
            clear_local_usages(usages, &usage_count, &has_usage_range, &usage_min, &usage_max);
            (void)logical_min;
        } else if (item_type == 0) {
            clear_local_usages(usages, &usage_count, &has_usage_range, &usage_min, &usage_max);
        }
    }

    fmt.valid = fmt.x_bit >= 0 && fmt.y_bit >= 0;
    s_report_format = fmt;
    ESP_LOGI(TAG,
             "Mouse descriptor parser: valid=%d report_id=%u has_id=%d buttons=%d/%d x=%d/%d y=%d/%d wheel=%d/%d",
             fmt.valid,
             fmt.report_id,
             fmt.has_report_id,
             fmt.buttons_bit,
             fmt.buttons_count,
             fmt.x_bit,
             fmt.x_size,
             fmt.y_bit,
             fmt.y_size,
             fmt.wheel_bit,
             fmt.wheel_size);
}

static void handle_mouse_report(const uint8_t *data, size_t len)
{
    mouse_report_t report = {0};
    if (is_logitech_g305_receiver()) {
        if (!parse_logitech_g305_report(data, len, &report)) {
            log_unparsed_g305_report(data, len);
            return;
        }
    } else if (!parse_descriptor_mouse_report(data, len, &report) && !parse_boot_mouse_report(data, len, &report)) {
        ESP_LOGW(TAG, "Ignoring short mouse report len=%u", (unsigned)len);
        return;
    }
    report.raw_len = len > sizeof(report.raw) ? sizeof(report.raw) : len;
    memcpy(report.raw, data, report.raw_len);

    xQueueSend(g_mouse_report_queue, &report, 0);
    post_mouse_report_event(&report);
}

static void hid_iface_cb(hid_host_device_handle_t hdev, const hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    uint8_t data[16];
    size_t len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_get_raw_input_report_data(hdev, data, sizeof(data), &len));
        handle_mouse_report(data, len);
        break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        post_kvm_event(KVM_EVENT_USB_MOUSE_DISCONNECTED);
        ESP_LOGI(TAG, "USB HID mouse disconnected");
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_close(hdev));
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;

    default:
        break;
    }
}

static bool is_mouse_interface(const hid_host_dev_params_t *params)
{
    return params->proto == HID_PROTOCOL_MOUSE;
}

static void hid_device_cb(hid_host_device_handle_t hdev, const hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params = {0};
    hid_host_dev_info_t info = {0};
    char product[HID_STR_DESC_MAX_LENGTH * 4] = {0};

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_get_params(hdev, &params));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_get_device_info(hdev, &info));
    s_device_vid = info.VID;
    s_device_pid = info.PID;
    post_usb_device_event(KVM_EVENT_USB_HID_INTERFACE_SEEN, info.VID, info.PID, params.proto);

    if (wcstombs(product, info.iProduct, sizeof(product) - 1) == (size_t)-1) {
        strncpy(product, "<unavailable>", sizeof(product) - 1);
    }

    ESP_LOGI(TAG,
             "HID connected VID=0x%04X PID=0x%04X product=\"%s\" iface=%u subclass=%u proto=%u",
             info.VID,
             info.PID,
             product,
             params.iface_num,
             params.sub_class,
             params.proto);

    if (!is_mouse_interface(&params)) {
        ESP_LOGW(TAG, "Ignoring non-mouse HID interface proto=%u", params.proto);
        return;
    }

    const hid_host_device_config_t cfg = {
        .callback = hid_iface_cb,
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_open(hdev, &cfg));
    if (is_logitech_g305_receiver()) {
        const esp_err_t protocol_result = hid_class_request_set_protocol(hdev, HID_REPORT_PROTOCOL_REPORT);
        if (protocol_result != ESP_OK) {
            ESP_LOGW(TAG, "G305 set report protocol returned %s; continuing", esp_err_to_name(protocol_result));
        }
    } else {
        size_t report_desc_len = 0;
        uint8_t *report_desc = hid_host_get_report_descriptor(hdev, &report_desc_len);
        if (report_desc != NULL && report_desc_len > 0) {
            parse_mouse_report_descriptor(report_desc, report_desc_len);
        } else {
            memset(&s_report_format, 0, sizeof(s_report_format));
            ESP_LOGW(TAG, "HID report descriptor unavailable; falling back to boot-like parser");
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_start(hdev));

    post_usb_device_event(KVM_EVENT_USB_MOUSE_CONNECTED, info.VID, info.PID, params.proto);
    ESP_LOGI(TAG, "USB HID mouse started");
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
        .enum_filter_cb = usb_enum_filter_cb,
#endif
    };

    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    post_kvm_event(KVM_EVENT_USB_HOST_STARTED);
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t usb_mouse_host_start(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    xTaskCreate(usb_lib_task, "usb_lib", 4096, self, 2, NULL);
    if (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000)) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for USB host library task");
        return ESP_ERR_TIMEOUT;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };

    esp_err_t result = hid_host_install(&hid_cfg);
    if (result == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "HID host driver already installed");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, TAG, "failed to install HID host driver");
    ESP_LOGI(TAG, "USB HID mouse host started");
    return ESP_OK;
}
