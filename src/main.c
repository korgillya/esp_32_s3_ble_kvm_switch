#include "app_config.h"
#include "app_queues.h"
#include "board_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "kvm_slots.h"
#include "tasks.h"

QueueHandle_t g_kvm_event_queue = NULL;
QueueHandle_t g_mouse_report_queue = NULL;

void app_main(void)
{
    ESP_LOGI(APP_NAME, "booting ESP32-S3 BLE KVM switch");

    ESP_ERROR_CHECK(kvm_slots_init());

    g_kvm_event_queue = xQueueCreate(KVM_EVENT_QUEUE_DEPTH, sizeof(kvm_event_t));
    g_mouse_report_queue = xQueueCreate(MOUSE_REPORT_QUEUE_DEPTH, sizeof(mouse_report_t));
    if (g_kvm_event_queue == NULL || g_mouse_report_queue == NULL) {
        ESP_LOGE(APP_NAME, "failed to create RTOS queues");
        return;
    }

    ESP_ERROR_CHECK(board_io_init());
    ESP_ERROR_CHECK(board_i2c_init());

    kvm_task_start();
    ui_task_start();
    usb_host_task_start();
    ble_hid_task_start();
}
