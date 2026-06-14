#include "ssd1306.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#define OLED_I2C_ADDR_PRIMARY 0x3C
#define OLED_I2C_ADDR_SECONDARY 0x3D
#define OLED_I2C_TIMEOUT_MS 100

static const char *TAG = "ssd1306";
static uint8_t s_oled_addr = OLED_I2C_ADDR_PRIMARY;
static bool s_ready;

typedef struct {
    char c;
    uint8_t glyph[5];
} glyph_t;

static const glyph_t FONT[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '!', { 0x00, 0x00, 0x5f, 0x00, 0x00 } },
    { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
    { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
    { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
    { '0', { 0x3e, 0x51, 0x49, 0x45, 0x3e } },
    { '1', { 0x00, 0x42, 0x7f, 0x40, 0x00 } },
    { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
    { '3', { 0x21, 0x41, 0x45, 0x4b, 0x31 } },
    { '4', { 0x18, 0x14, 0x12, 0x7f, 0x10 } },
    { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
    { '6', { 0x3c, 0x4a, 0x49, 0x49, 0x30 } },
    { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
    { '9', { 0x06, 0x49, 0x49, 0x29, 0x1e } },
    { 'A', { 0x7e, 0x11, 0x11, 0x11, 0x7e } },
    { 'B', { 0x7f, 0x49, 0x49, 0x49, 0x36 } },
    { 'C', { 0x3e, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7f, 0x41, 0x41, 0x22, 0x1c } },
    { 'E', { 0x7f, 0x49, 0x49, 0x49, 0x41 } },
    { 'F', { 0x7f, 0x09, 0x09, 0x09, 0x01 } },
    { 'G', { 0x3e, 0x41, 0x49, 0x49, 0x7a } },
    { 'H', { 0x7f, 0x08, 0x08, 0x08, 0x7f } },
    { 'I', { 0x00, 0x41, 0x7f, 0x41, 0x00 } },
    { 'J', { 0x20, 0x40, 0x41, 0x3f, 0x01 } },
    { 'K', { 0x7f, 0x08, 0x14, 0x22, 0x41 } },
    { 'L', { 0x7f, 0x40, 0x40, 0x40, 0x40 } },
    { 'M', { 0x7f, 0x02, 0x0c, 0x02, 0x7f } },
    { 'N', { 0x7f, 0x04, 0x08, 0x10, 0x7f } },
    { 'O', { 0x3e, 0x41, 0x41, 0x41, 0x3e } },
    { 'P', { 0x7f, 0x09, 0x09, 0x09, 0x06 } },
    { 'Q', { 0x3e, 0x41, 0x51, 0x21, 0x5e } },
    { 'R', { 0x7f, 0x09, 0x19, 0x29, 0x46 } },
    { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'T', { 0x01, 0x01, 0x7f, 0x01, 0x01 } },
    { 'U', { 0x3f, 0x40, 0x40, 0x40, 0x3f } },
    { 'V', { 0x1f, 0x20, 0x40, 0x20, 0x1f } },
    { 'W', { 0x7f, 0x20, 0x18, 0x20, 0x7f } },
    { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
    { 'Y', { 0x03, 0x04, 0x78, 0x04, 0x03 } },
    { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
};

static const uint8_t *find_glyph(char c)
{
    const char upper = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); ++i) {
        if (FONT[i].c == upper) {
            return FONT[i].glyph;
        }
    }
    return FONT[0].glyph;
}

static esp_err_t oled_write(uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t buffer[17];
    size_t offset = 0;

    while (offset < len) {
        const size_t chunk = (len - offset > 16) ? 16 : len - offset;
        buffer[0] = control;
        memcpy(&buffer[1], data + offset, chunk);
        ESP_RETURN_ON_ERROR(
            i2c_master_write_to_device(I2C_PORT, s_oled_addr, buffer, chunk + 1, pdMS_TO_TICKS(OLED_I2C_TIMEOUT_MS)),
            TAG,
            "i2c write");
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    return oled_write(0x00, &cmd, 1);
}

static esp_err_t oled_probe_addr(uint8_t addr)
{
    uint8_t cmd = 0x00;
    return i2c_master_write_to_device(I2C_PORT, addr, &cmd, 1, pdMS_TO_TICKS(OLED_I2C_TIMEOUT_MS));
}

static esp_err_t oled_probe(void)
{
    esp_err_t result = oled_probe_addr(OLED_I2C_ADDR_PRIMARY);
    if (result == ESP_OK) {
        s_oled_addr = OLED_I2C_ADDR_PRIMARY;
        return ESP_OK;
    }

    result = oled_probe_addr(OLED_I2C_ADDR_SECONDARY);
    if (result == ESP_OK) {
        s_oled_addr = OLED_I2C_ADDR_SECONDARY;
        return ESP_OK;
    }

    return result;
}

esp_err_t ssd1306_init(ssd1306_t *display)
{
    static const uint8_t init_cmds[] = {
        0xAE,       // display off
        0x20, 0x00, // horizontal addressing
        0xB0,
        0xC8,
        0x00,
        0x10,
        0x40,
        0x81, 0x7F,
        0xA1,
        0xA6,
        0xA8, 0x3F,
        0xA4,
        0xD3, 0x00,
        0xD5, 0x80,
        0xD9, 0xF1,
        0xDA, 0x12,
        0xDB, 0x40,
        0x8D, 0x14,
        0xAF, // display on
    };

    ESP_RETURN_ON_ERROR(oled_probe(), TAG, "OLED not found at 0x3C or 0x3D");
    for (size_t i = 0; i < sizeof(init_cmds); ++i) {
        ESP_RETURN_ON_ERROR(oled_send_cmd(init_cmds[i]), TAG, "init cmd");
    }

    s_ready = true;
    ssd1306_clear(display);
    return ssd1306_present(display);
}

esp_err_t ssd1306_present(const ssd1306_t *display)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "display not ready");

    ESP_RETURN_ON_ERROR(oled_send_cmd(0x21), TAG, "col mode");
    ESP_RETURN_ON_ERROR(oled_send_cmd(0), TAG, "col start");
    ESP_RETURN_ON_ERROR(oled_send_cmd(SSD1306_WIDTH - 1), TAG, "col end");
    ESP_RETURN_ON_ERROR(oled_send_cmd(0x22), TAG, "page mode");
    ESP_RETURN_ON_ERROR(oled_send_cmd(0), TAG, "page start");
    ESP_RETURN_ON_ERROR(oled_send_cmd((SSD1306_HEIGHT / 8) - 1), TAG, "page end");
    return oled_write(0x40, display->buffer, sizeof(display->buffer));
}

void ssd1306_clear(ssd1306_t *display)
{
    memset(display->buffer, 0, sizeof(display->buffer));
}

void ssd1306_set_pixel(ssd1306_t *display, int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }

    const size_t index = (size_t)x + ((size_t)y / 8U) * SSD1306_WIDTH;
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) {
        display->buffer[index] |= mask;
    } else {
        display->buffer[index] &= (uint8_t)~mask;
    }
}

void ssd1306_draw_rect(ssd1306_t *display, int x, int y, int w, int h, bool on)
{
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            ssd1306_set_pixel(display, x + xx, y + yy, on);
        }
    }
}

void ssd1306_draw_frame(ssd1306_t *display, int x, int y, int w, int h, bool on)
{
    for (int xx = 0; xx < w; ++xx) {
        ssd1306_set_pixel(display, x + xx, y, on);
        ssd1306_set_pixel(display, x + xx, y + h - 1, on);
    }
    for (int yy = 0; yy < h; ++yy) {
        ssd1306_set_pixel(display, x, y + yy, on);
        ssd1306_set_pixel(display, x + w - 1, y + yy, on);
    }
}

static void draw_char(ssd1306_t *display, int x, int y, char c, bool on, uint8_t scale)
{
    const uint8_t *glyph = find_glyph(c);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[col] >> row) & 0x01) {
                ssd1306_draw_rect(display, x + col * scale, y + row * scale, scale, scale, on);
            }
        }
    }
}

void ssd1306_draw_text(ssd1306_t *display, int x, int y, const char *text, bool on, uint8_t scale)
{
    int cursor_x = x;
    if (scale == 0) {
        scale = 1;
    }

    while (*text) {
        draw_char(display, cursor_x, y, *text, on, scale);
        cursor_x += (6 * scale);
        ++text;
    }
}
