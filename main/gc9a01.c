Minimal GC9A01 panel driver using esp_lcd (SPI). Provides init + a few draw helpers. */
#include <string.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "gc9a01.h"

#define LCD_H 240
#define LCD_W 240

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static int s_bl_gpio = -1;

esp_err_t gc9a01_init(int sck, int mosi, int dc, int cs, int rst, int bl)
{
    s_bl_gpio = bl;
    if (s_bl_gpio >= 0) {
        gpio_config_t io_conf = { .pin_bit_mask = 1ULL << s_bl_gpio, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io_conf);
        gpio_set_level(s_bl_gpio, 0);
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = sck,
        .mosi_io_num = mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = dc,
        .cs_gpio_num = cs,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = rst,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(s_io, &panel_config, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    if (s_bl_gpio >= 0) {
        gpio_set_level(s_bl_gpio, 1);
    }
    return ESP_OK;
}

void gc9a01_fill(uint16_t color)
{
    static uint16_t line[LCD_W];
    for (int x=0; x<LCD_W; ++x) line[x] = color;
    for (int y=0; y<LCD_H; ++y) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y+1, line);
    }
}

void gc9a01_draw_bitmap(int x1, int y1, int x2, int y2, const void *data)
{
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, data);
}

// Tiny text rendering using 6x8 font
#include "font6x8.h"
static inline void putpix(int x, int y, uint16_t c){ if (x<0||y<0||x>=LCD_W||y>=LCD_H) return; esp_lcd_panel_draw_bitmap(s_panel,x,y,x+1,y+1,&c);} 

void draw_char6x8(int x, int y, char ch, uint16_t color)
{
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *col = g_font6x8[ch-32];
    for (int cx=0; cx<6; ++cx){
        uint8_t m = col[cx];
        for (int cy=0; cy<8; ++cy){ if (m & (1<<cy)) putpix(x+cx, y+cy, color); }
    }
}

void draw_text6x8(int x, int y, const char *s, uint16_t color)
{
    for (; *s; ++s){
        if (*s=='\n'){ y += 10; x = 0; continue; }
        draw_char6x8(x,y,*s,color); x += 7;
    }
}

// Simple helpers
uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){ return ((r&0xF8)<<8)|((g&0xFC)<<3)|((b)>>3); }

