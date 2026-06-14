#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern QueueHandle_t g_kvm_event_queue;
extern QueueHandle_t g_mouse_report_queue;
