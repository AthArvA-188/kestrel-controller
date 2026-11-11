// ST7789 display, SK6812 chain, MAX17048 fuel gauge, and NVS storage.
#include <string.h>

#include "app.h"
#include "driver/i2c_master.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ks_periph.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pins_gen.h"

static const char *TAG = "periph";

// Which way up the panel sits depends on the enclosure. Flip both to rotate
// the picture by 180 degrees.
#define LCD_MIRROR_X 0
#define LCD_MIRROR_Y 1
#define LCD_STRIPS 5
#define LCD_STRIP_ROWS (LCD_HEIGHT / LCD_STRIPS)

#define MAX17048_REG_VCELL 0x02
#define MAX17048_REG_SOC 0x04

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t lcd_done;
static uint16_t *framebuffer;
static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_encoder;
static uint8_t led_frame[KS_LED_COUNT * 3];
static i2c_master_dev_handle_t fuel_dev;

static bool lcd_strip_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *data,
                           void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(lcd_done, &woken);
    return woken == pdTRUE;
}

static void display_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_STRIP_ROWS * 2,
    };
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = LCD_SPI_MODE,
        .trans_queue_depth = LCD_STRIPS,
        .on_color_trans_done = lcd_strip_done,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_io_handle_t io;

    lcd_done = xSemaphoreCreateCounting(LCD_STRIPS, 0);
    // The SPI driver sends straight from this buffer, so it must be DMA capable.
    framebuffer = heap_caps_calloc(LCD_WIDTH * LCD_HEIGHT, sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(lcd_done && framebuffer);

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true)); // IPS panels need inversion on
    // The panel is 170 by 320 in portrait and its 170 columns start at RAM
    // column 35. Landscape swaps the axes, which moves that offset to y.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, LCD_RAM_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}

uint16_t *display_buffer(void) { return framebuffer; }

static void swap_bytes(uint16_t *px, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8));
    }
}

void display_flush(void)
{
    // The panel takes the high byte first. Swap in place, send, and swap back
    // once every strip has gone out, which saves a second 109 kB buffer.
    swap_bytes(framebuffer, LCD_WIDTH * LCD_HEIGHT);
    int queued = 0;
    for (int s = 0; s < LCD_STRIPS; s++) {
        int y = s * LCD_STRIP_ROWS;
        if (esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH, y + LCD_STRIP_ROWS,
                                      framebuffer + y * LCD_WIDTH) == ESP_OK) {
            queued++;
        }
    }
    for (int s = 0; s < queued; s++) {
        xSemaphoreTake(lcd_done, pdMS_TO_TICKS(200));
    }
    swap_bytes(framebuffer, LCD_WIDTH * LCD_HEIGHT);
}

static void leds_init(void)
{
    rmt_tx_channel_config_t chan = {
        .gpio_num = PIN_LED_DATA,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 0.1 microsecond ticks
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    // SK6812 bit timing: a 0 is 0.3 us high then 0.9 us low, a 1 is 0.6 us
    // high then 0.6 us low. The line rests low between frames, which is the
    // reset.
    rmt_bytes_encoder_config_t enc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6},
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan, &led_chan));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

void leds_show(const uint8_t *grb)
{
    rmt_transmit_config_t cfg = {.loop_count = 0};
    if (memcmp(led_frame, grb, sizeof led_frame) == 0) {
        return;
    }
    // The driver reads the buffer while it sends, so let the last frame finish.
    rmt_tx_wait_all_done(led_chan, 20);
    memcpy(led_frame, grb, sizeof led_frame);
    rmt_transmit(led_chan, led_encoder, led_frame, sizeof led_frame, &cfg);
}

static void fuel_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, // the board has its own 4.7k pull-ups
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_MAX17048,
        .scl_speed_hz = I2C_BUS_HZ,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &fuel_dev));
}

static bool fuel_reg(uint8_t reg, uint16_t *value)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(fuel_dev, &reg, 1, rx, 2, 50) != ESP_OK) {
        return false;
    }
    *value = (uint16_t)((rx[0] << 8) | rx[1]);
    return true;
}

bool fuel_read(uint8_t *pct, uint16_t *mv)
{
    uint16_t soc, vcell;
    if (!fuel_reg(MAX17048_REG_SOC, &soc) || !fuel_reg(MAX17048_REG_VCELL, &vcell)) {
        return false;
    }
    *pct = ks_fuel_soc_percent(soc);
    *mv = ks_fuel_vcell_mv(vcell);
    return true;
}

static bool nvs_blob_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    nvs_handle_t h;
    size_t n = cap;
    if (nvs_open("kestrel", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    *len = n;
    return err == ESP_OK;
}

static bool nvs_blob_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("kestrel", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

const ks_storage_t *storage_get(void)
{
    static const ks_storage_t storage = {nvs_blob_read, nvs_blob_write, NULL};
    return &storage;
}

void storage_erase_namespace(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(name, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void periph_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    display_init();
    leds_init();
    fuel_init();
}
