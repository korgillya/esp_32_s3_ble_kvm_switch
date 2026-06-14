#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64
#define SSD1306_BUFFER_SIZE (SSD1306_WIDTH * SSD1306_HEIGHT / 8)

typedef struct {
    uint8_t buffer[SSD1306_BUFFER_SIZE];
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *display);
esp_err_t ssd1306_present(const ssd1306_t *display);
void ssd1306_clear(ssd1306_t *display);
void ssd1306_set_pixel(ssd1306_t *display, int x, int y, bool on);
void ssd1306_draw_rect(ssd1306_t *display, int x, int y, int w, int h, bool on);
void ssd1306_draw_frame(ssd1306_t *display, int x, int y, int w, int h, bool on);
void ssd1306_draw_text(ssd1306_t *display, int x, int y, const char *text, bool on, uint8_t scale);
