#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_commands.h"

static const char *TAG = "st7789_text_fix";

// ================= PIN CONFIG =================
#define PIN_NUM_MOSI     6
#define PIN_NUM_CLK      4
#define PIN_NUM_LCD_CS   7
#define PIN_NUM_LCD_DC   8
#define PIN_NUM_LCD_RST  9
#define PIN_NUM_LCD_BL   10

// ================= LCD CONFIG =================
#define LCD_HOST                SPI2_HOST
#define LCD_H_RES               240
#define LCD_V_RES               320
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8
#define LCD_PIXEL_CLOCK_HZ      (10 * 1000 * 1000)

#define LCD_X_GAP               0
#define LCD_Y_GAP               0
#define BYTES_PER_PIXEL         2

// Nếu màu còn sai thì đổi 1 <-> 0
#define LCD_USE_BGR             1

// Nếu màu còn bị ngược byte thì đổi 0 -> 1
#define LCD_SWAP_COLOR_BYTES    0

// ================= COLORS RGB565 =================
#define COLOR_BLACK             0x0000
#define COLOR_WHITE             0xFFFF
#define COLOR_RED               0xF800
#define COLOR_GREEN             0x07E0
#define COLOR_BLUE              0x001F
#define COLOR_YELLOW            0xFFE0
#define COLOR_CYAN              0x07FF
#define COLOR_MAGENTA           0xF81F
#define COLOR_GRAY              0x8410

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;

// =====================================================
// FONT 5x7 DẠNG HÀNG
// Mỗi row là 5 bit thấp
// =====================================================
typedef struct {
    char c;
    uint8_t row[7];
} font5x7_row_t;

static const font5x7_row_t font_table[] = {
    { ' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00} },
    { '-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00} },
    { '.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C} },

    { '0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E} },
    { '1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E} },
    { '2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F} },
    { '3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E} },
    { '4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02} },
    { '5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E} },
    { '6', {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E} },
    { '7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08} },
    { '8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E} },
    { '9', {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E} },

    { 'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E} },
    { 'C', {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F} },
    { 'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E} },
    { 'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F} },
    { 'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10} },
    { 'G', {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F} },
    { 'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E} },
    { 'J', {0x01,0x01,0x01,0x01,0x11,0x11,0x0E} },
    { 'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11} },
    { 'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F} },
    { 'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11} },
    { 'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11} },
    { 'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10} },
    { 'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D} },
    { 'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11} },
    { 'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E} },
    { 'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04} },
    { 'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04} },
    { 'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A} },
    { 'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11} },
    { 'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04} },
    { 'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F} },
};

static inline uint16_t lcd_fix_color(uint16_t c)
{
#if LCD_SWAP_COLOR_BYTES
    return (uint16_t)((c >> 8) | (c << 8));
#else
    return c;
#endif
}

static const uint8_t *find_char_rows(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)toupper((unsigned char)c);
    }

    for (size_t i = 0; i < sizeof(font_table) / sizeof(font_table[0]); i++) {
        if (font_table[i].c == c) {
            return font_table[i].row;
        }
    }

    return font_table[0].row;
}

static void lcd_send_cmd(uint8_t cmd)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, cmd, NULL, 0));
}

static void lcd_send_data(uint8_t cmd, const void *data, size_t len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, cmd, data, len));
}

// =====================================================
// LOW LEVEL
// =====================================================
static void lcd_backlight_init(void)
{
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(PIN_NUM_LCD_BL, 1);
}

static void lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * BYTES_PER_PIXEL,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
#if LCD_USE_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Ép mode RGB565
    uint8_t colmod = 0x55;
    lcd_send_data(0x3A, &colmod, 1);

    // MADCTL
#if LCD_USE_BGR
    uint8_t madctl = 0x08;
#else
    uint8_t madctl = 0x00;
#endif
    lcd_send_data(0x36, &madctl, 1);

    // Một số panel ST7789 nhìn đẹp hơn khi invert ON
    lcd_send_cmd(0x21);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Màn dọc 240x320
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, LCD_X_GAP, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "lcd init done");
}

// =====================================================
// DRAW BASIC
// =====================================================
static void lcd_fill_screen(uint16_t color)
{
    static uint16_t *buf = NULL;
    const int line_count = 20;
    const int buf_pixels = LCD_H_RES * line_count;
    uint16_t c = lcd_fix_color(color);

    if (!buf) {
        buf = heap_caps_malloc(buf_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
        assert(buf);
    }

    for (int i = 0; i < buf_pixels; i++) {
        buf[i] = c;
    }

    for (int y = 0; y < LCD_V_RES; y += line_count) {
        int h = line_count;
        if (y + h > LCD_V_RES) {
            h = LCD_V_RES - y;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + h, buf));
    }
}

static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x >= LCD_H_RES || y >= LCD_V_RES) return;
    if (x + w <= 0 || y + h <= 0) return;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;

    if (w <= 0 || h <= 0) return;

    uint16_t *buf = heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(buf);

    uint16_t c = lcd_fix_color(color);
    for (int i = 0; i < w * h; i++) {
        buf[i] = c;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, buf));
    free(buf);
}

static void lcd_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
    lcd_fill_rect(x, y + h - 1, w, 1, color);
    lcd_fill_rect(x, y, 1, h, color);
    lcd_fill_rect(x + w - 1, y, 1, h, color);
}

// =====================================================
// TEXT
// =====================================================
static void lcd_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (scale <= 0) return;

    const uint8_t *rows = find_char_rows(ch);
    const int src_w = 5;
    const int src_h = 7;
    const int dst_w = src_w * scale;
    const int dst_h = src_h * scale;

    uint16_t *buf = heap_caps_malloc(dst_w * dst_h * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(buf);

    uint16_t fg_fixed = lcd_fix_color(fg);
    uint16_t bg_fixed = lcd_fix_color(bg);

    for (int row = 0; row < dst_h; row++) {
        for (int col = 0; col < dst_w; col++) {
            int src_y = row / scale;
            int src_x = col / scale;

            bool on = ((rows[src_y] >> (4 - src_x)) & 0x01) ? true : false;
            buf[row * dst_w + col] = on ? fg_fixed : bg_fixed;
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + dst_w, y + dst_h, buf));
    free(buf);
}

static void lcd_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!text) return;

    int cursor_x = x;
    int cursor_y = y;
    const int char_w = 5 * scale;
    const int char_h = 7 * scale;
    const int spacing_x = 1 * scale;
    const int spacing_y = 2 * scale;

    while (*text) {
        if (*text == '\n') {
            cursor_x = x;
            cursor_y += char_h + spacing_y;
        } else {
            lcd_draw_char(cursor_x, cursor_y, *text, fg, bg, scale);
            cursor_x += char_w + spacing_x;
        }
        text++;
    }
}

// =====================================================
// DEMO
// =====================================================
static void lcd_test_colors(void)
{
    lcd_fill_screen(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(700));

    lcd_fill_screen(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(700));

    lcd_fill_screen(COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(700));

    lcd_fill_screen(COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(700));

    lcd_fill_screen(COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void lcd_draw_demo(void)
{
    lcd_fill_screen(COLOR_BLACK);

    lcd_draw_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOR_WHITE);

    lcd_fill_rect(18, 25, 204, 50, COLOR_BLACK);
    lcd_draw_text(20, 30, "HELLO", COLOR_GREEN, COLOR_BLACK, 4);

    lcd_fill_rect(18, 110, 204, 50, COLOR_RED);
    lcd_draw_text(24, 120, "ESP32-C3", COLOR_CYAN, COLOR_RED, 4);

    lcd_fill_rect(18, 220, 204, 36, COLOR_RED);
    lcd_draw_text(28, 230, "ST7789 240X320", COLOR_WHITE, COLOR_RED, 2);
}

// =====================================================
// MAIN
// =====================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Backlight init");
    lcd_backlight_init();

    ESP_LOGI(TAG, "LCD init");
    lcd_init();

    ESP_LOGI(TAG, "Start color test");
    lcd_test_colors();

    ESP_LOGI(TAG, "Draw demo");
    lcd_draw_demo();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}   