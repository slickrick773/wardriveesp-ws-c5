// WardriveESP — color status screens over SPI: the Koko Marauder V8's 240x320 ILI9341, the Marauder Mini v3's
// 128x128 ST7735 and the Waveshare ESP32-C6-LCD-1.47's 172x320 ST7789. Boot splash, live dashboard, alerts (scenes in lcd_draw.c). Drawn in 16-row strips into one small DMA
// buffer; a strip goes to the panel only when its pixels changed.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_efuse.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#endif
#include "lcd.h"
#include "lcd_draw.h"

static const char *TAG = "WardriveESP";

// Each board's bus: panel, microSD slot (and the Koko's XPT2046 touch). -1 = not on this board.
typedef struct { int miso, mosi, sclk, cs, dc, rst, touch_cs, sd_cs; } pins_t;
// Koko V8, Mini v3, AWOK Dual C5 (ESP32-C5): one layout on 6 / 2 / 7.
static const pins_t PINS_C5 = { 2, 7, 6, 23, 24, -1, 9, 10 };
// Waveshare ESP32-C6-LCD-1.47: panel and microSD share MOSI 6 / SCLK 7, the card's MISO on 5; the panel has a reset line.
__attribute__((unused)) static const pins_t PINS_WS147 = { 5, 6, 7, 14, 15, 21, -1, 4 };
// Waveshare ESP32-C5-LCD-1.47: the same board on the C5, panel CS 23 / DC 24 / reset 26 (backlight on 10).
__attribute__((unused)) static const pins_t PINS_WS147_C5 = { 5, 6, 7, 23, 24, 26, -1, 4 };
static const pins_t *s_pins = &PINS_C5;
#define PIN_MISO     (s_pins->miso)
#define PIN_MOSI     (s_pins->mosi)
#define PIN_SCLK     (s_pins->sclk)
#define PIN_CS       (s_pins->cs)
#define PIN_DC       (s_pins->dc)
#define PIN_RST      (s_pins->rst)
#define PIN_TOUCH_CS (s_pins->touch_cs)
#define PIN_SD_CS    (s_pins->sd_cs)
#define HOST         SPI2_HOST
#define CLK_READ_HZ  1000000
#define CLK_WRITE_HZ 20000000

#define SH LCD_SH
#define FRAME_MS     200
#define ALERT_BLINK_MS 250
#define BL_ON   255   // always full: an offline board is still working, and the screen is how you tell

typedef struct { uint8_t c, n; uint8_t p[16]; uint16_t ms; } init_t;

// ILI9341 power-up (no reset line: software reset), 16-bit color, portrait with BGR order.
__attribute__((unused)) static const init_t INIT_ILI9341[] = {
    { 0x01, 0, {0}, 150 },
    { 0xCF, 3, { 0x00, 0xC1, 0x30 }, 0 },
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 }, 0 },
    { 0xE8, 3, { 0x85, 0x00, 0x78 }, 0 },
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 }, 0 },
    { 0xF7, 1, { 0x20 }, 0 },
    { 0xEA, 2, { 0x00, 0x00 }, 0 },
    { 0xC0, 1, { 0x23 }, 0 },             // power control 1
    { 0xC1, 1, { 0x10 }, 0 },             // power control 2
    { 0xC5, 2, { 0x3E, 0x28 }, 0 },       // VCOM 1
    { 0xC7, 1, { 0x86 }, 0 },             // VCOM 2
    { 0x36, 1, { 0x48 }, 0 },             // MADCTL: MX + BGR (portrait)
    { 0x3A, 1, { 0x55 }, 0 },             // 16 bits per pixel
    { 0xB1, 2, { 0x00, 0x18 }, 0 },       // frame rate
    { 0xB6, 3, { 0x08, 0x82, 0x27 }, 0 }, // display function
    { 0xF2, 1, { 0x00 }, 0 },
    { 0x26, 1, { 0x01 }, 0 },             // gamma curve 1
    { 0xE0, 15, { 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00 }, 0 },
    { 0xE1, 15, { 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F }, 0 },
    { 0x20, 0, {0}, 0 },                  // inversion off
    { 0x11, 0, {0}, 120 },                // sleep out
};

// ST7735 (green tab, 128x128) power-up, the same register values the board's stock firmware sends.
__attribute__((unused)) static const init_t INIT_ST7735[] = {
    { 0x01, 0, {0}, 150 },                // software reset
    { 0x11, 0, {0}, 255 },                // sleep out
    { 0xB1, 3, { 0x01, 0x2C, 0x2D }, 0 }, // frame rate: normal
    { 0xB2, 3, { 0x01, 0x2C, 0x2D }, 0 }, // idle
    { 0xB3, 6, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D }, 0 },   // partial
    { 0xB4, 1, { 0x07 }, 0 },             // no inversion
    { 0xC0, 3, { 0xA2, 0x02, 0x84 }, 0 }, // power control 1-5
    { 0xC1, 1, { 0xC5 }, 0 },
    { 0xC2, 2, { 0x0A, 0x00 }, 0 },
    { 0xC3, 2, { 0x8A, 0x2A }, 0 },
    { 0xC4, 2, { 0x8A, 0xEE }, 0 },
    { 0xC5, 1, { 0x0E }, 0 },             // VCOM
    { 0x20, 0, {0}, 0 },                  // inversion off
    { 0x36, 1, { 0xC8 }, 0 },             // MADCTL: MX + MY + BGR
    { 0x3A, 1, { 0x05 }, 0 },             // 16 bits per pixel
    { 0xE0, 16, { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 }, 0 },
    { 0xE1, 16, { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 }, 0 },
    { 0x13, 0, {0}, 10 },                 // normal display on
};

// ST7789 (Waveshare 1.47" IPS, 172x320 in the controller's 240x320): portrait, RGB order, 16-bit color, inverted
// (an IPS panel shows true colors with inversion on).
__attribute__((unused)) static const init_t INIT_ST7789[] = {
    { 0x01, 0, {0}, 150 },                // software reset
    { 0x11, 0, {0}, 120 },                // sleep out
    { 0x36, 1, { 0x00 }, 0 },             // MADCTL: portrait, RGB
    { 0x3A, 1, { 0x55 }, 0 },             // 16 bits per pixel
    { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 0 },   // porch
    { 0xB7, 1, { 0x35 }, 0 },             // gate control
    { 0xBB, 1, { 0x35 }, 0 },             // VCOM
    { 0xC0, 1, { 0x2C }, 0 },
    { 0xC2, 1, { 0x01 }, 0 },
    { 0xC3, 1, { 0x13 }, 0 },             // VRH
    { 0xC4, 1, { 0x20 }, 0 },             // VDV
    { 0xC6, 1, { 0x0F }, 0 },             // 60 Hz
    { 0xD0, 2, { 0xA4, 0xA1 }, 0 },       // power control
    { 0xE0, 14, { 0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30 }, 0 },
    { 0xE1, 14, { 0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32 }, 0 },
    { 0x21, 0, {0}, 0 },                  // inversion on
    { 0x13, 0, {0}, 10 },                 // normal display on
};

// ST7796 (Waveshare ESP32-S3-Touch-LCD-3.5, 320x480 IPS): Waveshare's own vendor sequence (its factory firmware's
// esp_lcd_st7796), portrait, BGR order, 16-bit color, inverted (IPS).
__attribute__((unused)) static const init_t INIT_ST7796[] = {
    { 0x01, 0, {0}, 120 },                // software reset
    { 0x11, 0, {0}, 120 },                // sleep out
    { 0x36, 1, { 0x48 }, 0 },             // MADCTL: portrait, mirrored in x (the factory firmware's mirror_x), BGR
    { 0x3A, 1, { 0x55 }, 0 },             // 16 bits per pixel
    { 0xF0, 1, { 0xC3 }, 0 },             // command set unlock
    { 0xF0, 1, { 0x96 }, 0 },
    { 0xB4, 1, { 0x01 }, 0 },
    { 0xB7, 1, { 0xC6 }, 0 },
    { 0xC0, 2, { 0x80, 0x45 }, 0 },
    { 0xC1, 1, { 0x13 }, 0 },
    { 0xC2, 1, { 0xA7 }, 0 },
    { 0xC5, 1, { 0x0A }, 0 },
    { 0xE8, 8, { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }, 0 },
    { 0xE0, 14, { 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31 }, 0 },
    { 0xE1, 14, { 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32 }, 0 },
    { 0xF0, 1, { 0x3C }, 0 },             // command set lock
    { 0xF0, 1, { 0x69 }, 120 },
    { 0x21, 0, {0}, 0 },                  // inversion on
};

typedef struct {
    const char *name, *size;
    int w, h, xofs, yofs;                 // panel size and where it sits in the controller's memory
    int bl;                               // backlight GPIO
    bool bl_low;                          // backlight on when the pin is low
    int hz;                               // write clock
    const init_t *init;
    size_t n_init;
} panel_t;

__attribute__((unused)) static const panel_t KOKO = { "240x320 ILI9341", "240x320", 240, 320, 0, 0, 8, false, CLK_WRITE_HZ, INIT_ILI9341,
                              sizeof(INIT_ILI9341) / sizeof(INIT_ILI9341[0]) };
__attribute__((unused)) static const panel_t MINI = { "128x128 ST7735", "128x128", 128, 128, 2, 3, 5, true, CLK_WRITE_HZ, INIT_ST7735,
                              sizeof(INIT_ST7735) / sizeof(INIT_ST7735[0]) };
__attribute__((unused)) static const panel_t WS147 = { "172x320 ST7789", "172x320", 172, 320, 34, 0, 22, false, 40000000,
                              INIT_ST7789, sizeof(INIT_ST7789) / sizeof(INIT_ST7789[0]) };
__attribute__((unused)) static const panel_t WS35 = { "320x480 ST7796", "320x480", 320, 480, 0, 0, 6, false, 40000000,
                              INIT_ST7796, sizeof(INIT_ST7796) / sizeof(INIT_ST7796[0]) };
__attribute__((unused)) static const panel_t WS147_C5 = { "172x320 ST7789", "172x320", 172, 320, 34, 0, 10, false, 40000000,
                              INIT_ST7789, sizeof(INIT_ST7789) / sizeof(INIT_ST7789[0]) };

static const panel_t *s_panel;
/// Either Waveshare 1.47 (C6 or C5): its card slot stays unused.
__attribute__((unused)) static bool is_ws147(void) { return s_panel == &WS147 || s_panel == &WS147_C5; }
static spi_device_handle_t s_dev;
static bool s_bus;
static bool s_sd_cs;                 // the microSD slot's chip select read pulled up when the bus opened
static SemaphoreHandle_t s_bus_lock; // the panel and the microSD card take turns on the bus (sdrun.c)
static uint16_t *s_strip;            // SH rows of pixels, byte-swapped for the wire
static uint32_t s_hash[LCD_STRIPS];  // what each strip on the panel holds now
static TaskHandle_t s_task;
static lcd_fill_fn s_fill;
static const char *s_board = "", *s_version = "";
static volatile bool s_stealth;
static volatile uint32_t s_alert_since, s_alert_until;
static char s_alert_label[13];
static volatile uint32_t s_alert_rgb;
static volatile bool s_alert_police;
static uint32_t s_t0;
static volatile uint8_t s_page;      // touch boards: which page shows (swipe), 0 = the dashboard
static bool s_touch_ok;              // a touch panel answered: the pages and the hints show
static lcd_touch_fn s_touch_fn;

// Channel activity from AP sightings (14 × 2.4 GHz, then 28 × 5 GHz). The counts themselves come from apcount via the
// fill callback; "+N new" is the session count now minus ten seconds ago.
static volatile uint8_t s_act[LCD_ACT_N];
static uint32_t s_session_at[10];   // session count at the start of each of the last ten seconds

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#if CONFIG_IDF_TARGET_ESP32S3
// The S3 3.5's panel goes through ESP-IDF's LCD panel IO, the way Waveshare's factory firmware drives it (a plain
// spi_master path sent the same bytes and the panel stayed dark on the bench, 2026-11-10).
static esp_lcd_panel_io_handle_t s_io;
static volatile bool s_io_busy;          // a strip is on the wire (DMA)
static uint16_t *s_io_buf[2];            // drawn into in turn: the next strip draws while the last one sends
static const uint16_t *s_io_inflight;
static bool io_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx) {
    s_io_busy = false;
    return false;
}
// A strip takes about a millisecond on the wire: spinning beats sleeping until the next tick (10 ms).
static void io_wait(void) {
    int64_t t0 = esp_timer_get_time();
    while (s_io_busy && esp_timer_get_time() - t0 < 100000) { }
    s_io_busy = false;
}
#endif

// ── SPI ──
static esp_err_t send(bool data, const void *buf, size_t n) {
    if (!n) return ESP_OK;
    gpio_set_level(PIN_DC, data);
    spi_transaction_t t = { .length = n * 8, .tx_buffer = buf };
    return spi_device_polling_transmit(s_dev, &t);
}
static esp_err_t cmd(uint8_t c, const uint8_t *p, size_t n) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_io) return esp_lcd_panel_io_tx_param(s_io, c, n ? p : NULL, n);
#endif
    esp_err_t e = send(false, &c, 1);
    return e == ESP_OK ? send(true, p, n) : e;
}

// Command `c`, then `n` (≤ 4) bytes back with CS held low across both. On a 3-wire device the answer comes back on MOSI.
__attribute__((unused)) static esp_err_t read_reg(uint8_t c, uint8_t *out, size_t n, bool three_wire) {
    if (spi_device_acquire_bus(s_dev, portMAX_DELAY) != ESP_OK) return ESP_FAIL;
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE };
    t.tx_data[0] = c;
    esp_err_t e = spi_device_polling_transmit(s_dev, &t);
    if (e == ESP_OK) {
        gpio_set_level(PIN_DC, 1);
        spi_transaction_t r = { .length = three_wire ? 0 : n * 8, .rxlength = n * 8,
                                .flags = SPI_TRANS_USE_RXDATA | (three_wire ? 0 : SPI_TRANS_USE_TXDATA) };
        e = spi_device_polling_transmit(s_dev, &r);
        memcpy(out, r.rx_data, n);
    }
    spi_device_release_bus(s_dev);
    return e;
}

__attribute__((unused)) static esp_err_t add_device(int hz, bool three_wire) {
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    spi_device_interface_config_t dc = { .clock_speed_hz = hz, .mode = 0, .spics_io_num = PIN_CS, .queue_size = 1,
                                         .flags = three_wire ? SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX : 0 };
    return spi_bus_add_device(HOST, &dc, &s_dev);
}

__attribute__((unused)) static void bus_release(void) {
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    if (s_bus) { spi_bus_free(HOST); s_bus = false; }
    s_sd_cs = false;
    const int PINS[] = { PIN_MISO, PIN_MOSI, PIN_SCLK, PIN_CS, PIN_DC, PIN_RST, PIN_TOUCH_CS, PIN_SD_CS };
    for (size_t i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) if (PINS[i] >= 0) gpio_reset_pin(PINS[i]);
}

// Input only, internal pull-down: a pin that still reads high has an external pull-up (a chip select's idle state).
__attribute__((unused)) static bool pulled_up(int gpio) {
    if (gpio < 0) return false;
    gpio_config_t io = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(200);
    int high = 0;
    for (int i = 0; i < 5; i++) { high += gpio_get_level(gpio); esp_rom_delay_us(50); }
    gpio_reset_pin(gpio);
    return high >= 4;
}

// Only when one of the bus's chip selects already reads pulled up: never drive these pins on a board without them.
// Opens the bus and holds every pulled-up select other than the panel's at its resting (deselected) level.
__attribute__((unused)) static bool bus_open(const char *who, bool touch_too) {
    bool cs = pulled_up(PIN_CS), tcs = touch_too && pulled_up(PIN_TOUCH_CS), sdcs = pulled_up(PIN_SD_CS);
    ESP_LOGI(TAG, "%s probe: pulled up CS %d=%d, touch CS %d=%d, SD CS %d=%d", who, PIN_CS, cs, PIN_TOUCH_CS, tcs,
             PIN_SD_CS, sdcs);
    if (!cs && !tcs && !sdcs) return false;
    if (tcs) { gpio_set_direction(PIN_TOUCH_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_TOUCH_CS, 1); }
    if (sdcs) { gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_SD_CS, 1); }
    s_sd_cs = sdcs;
    if (sdcs) {
        // A microSD card that the last boot put in SPI mode (a warm reset never powers it down) can keep driving MISO
        // and garble the panel's ID read. Clocks with every select high make it let go of the line.
        gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_CS, 1);
        gpio_set_direction(PIN_MOSI, GPIO_MODE_OUTPUT); gpio_set_level(PIN_MOSI, 1);
        gpio_set_direction(PIN_SCLK, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 80; i++) {
            gpio_set_level(PIN_SCLK, 1); esp_rom_delay_us(4);
            gpio_set_level(PIN_SCLK, 0); esp_rom_delay_us(4);
        }
    }
    spi_bus_config_t bc = { .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * SH * 2 + 16 };
    if (spi_bus_initialize(HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) { bus_release(); return false; }
    s_bus = true;
    gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DC, 1);
    return true;
}

__attribute__((unused)) static bool is_ili9341(const uint8_t *id) { return id[1] == 0x93 && id[2] == 0x41; }

bool lcd_probe_koko(bool stored) {
#if !CONFIG_IDF_TARGET_ESP32C5
    (void)stored;
    return false;
#else
    if (s_bus) return s_panel == &KOKO;
    if (!bus_open("koko lcd", true)) return false;
    if (add_device(CLK_READ_HZ, false) != ESP_OK) { bus_release(); return false; }
    uint8_t id[4] = {0};
    read_reg(0xD3, id, 4, false);   // ID4: dummy, 00, 93, 41
    bool ok = is_ili9341(id + 1) || is_ili9341(id);
    if (!ok) {
        // Panels that only answer ID4 one byte at a time, indexed through the undocumented 0xD9 register.
        uint8_t b[3] = {0};
        for (int i = 0; i < 3; i++) {
            const uint8_t idx = (uint8_t)(0x10 + i + 1);
            cmd(0xD9, &idx, 1);
            uint8_t v = 0;
            read_reg(0xD3, &v, 1, false);
            b[i] = v;
        }
        ESP_LOGI(TAG, "koko lcd probe: ID4 indexed %02x %02x %02x", b[0], b[1], b[2]);
        ok = is_ili9341(b);
    }
    ESP_LOGI(TAG, "koko lcd probe: ID4 %02x %02x %02x %02x -> %s", id[0], id[1], id[2], id[3],
             ok ? "ILI9341" : stored ? "no ID read, picked by hand" : "none");
    // A board picked by hand (`board koko_v8` / `board awok_dual_c5`) drives the panel even when it can't be read back.
    if ((!ok && !stored) || add_device(KOKO.hz, false) != ESP_OK) { bus_release(); return false; }
    s_panel = &KOKO;
    return true;
#endif
}

bool lcd_probe_mini(bool stored) {
#if !CONFIG_IDF_TARGET_ESP32C5
    (void)stored;
    return false;
#else
    if (s_bus) return s_panel == &MINI;
    if (!bus_open("mini lcd", false)) return false;
    // The ST7735 answers only on its one data line: 3-wire on the bus's MOSI. RDID1 (DAh) is one byte with no dummy
    // clock; RDDID (04h) is one dummy clock, then 24 bits. Some modules also bring it out on MISO (4-wire).
    uint8_t r[4] = {0};
    bool ok = false;
    if (add_device(CLK_READ_HZ / 2, true) == ESP_OK) {
        read_reg(0xDA, r, 1, true);
        ok = r[0] == 0x7C;
        ESP_LOGI(TAG, "mini lcd probe: RDID1 3-wire %02x", r[0]);
        if (!ok) {
            read_reg(0x04, r, 4, true);
            uint32_t raw = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
            ok = ((raw >> 23) & 0xFF) == 0x7C;
            ESP_LOGI(TAG, "mini lcd probe: RDDID 3-wire raw %08lx", (unsigned long)raw);
        }
    }
    if (!ok && add_device(CLK_READ_HZ / 2, false) == ESP_OK) {
        read_reg(0xDA, r, 1, false);
        ok = r[0] == 0x7C;
        ESP_LOGI(TAG, "mini lcd probe: RDID1 4-wire %02x", r[0]);
    }
    // A panel that can't be read back still counts when the board was told it is a Mini (`board marauder_mini_v3`).
    ESP_LOGI(TAG, "mini lcd probe: %s", ok ? "ST7735" : stored ? "no ID read, stored identity says Mini" : "none");
    if ((!ok && !stored) || add_device(MINI.hz, false) != ESP_OK) { bus_release(); return false; }
    s_panel = &MINI;
    return true;
#endif
}

// What a pin reads with each internal pull, never driven: L = held low, H = held high, F = floating (follows the pull).
__attribute__((unused)) static char pin_state(int gpio) {
    int v[2];
    for (int k = 0; k < 2; k++) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_INPUT,
                             .pull_up_en = k ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                             .pull_down_en = k ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE };
        gpio_config(&io);
        esp_rom_delay_us(200);
        int high = 0;
        for (int i = 0; i < 5; i++) { high += gpio_get_level(gpio); esp_rom_delay_us(50); }
        v[k] = high >= 3;
    }
    gpio_reset_pin(gpio);
    return v[0] && v[1] ? 'H' : !v[0] && !v[1] ? 'L' : 'F';
}

static char s_probe_note[112];   // what the last board probe saw, for `board`
const char *lcd_probe_note(void) { return s_probe_note; }

bool lcd_probe_waveshare(bool stored) {
#if !CONFIG_IDF_TARGET_ESP32C6 && !CONFIG_IDF_TARGET_ESP32C5
    (void)stored;
    return false;
#else
    if (s_bus) return is_ws147();
#if CONFIG_IDF_TARGET_ESP32C6
    const panel_t *panel = &WS147;
    s_pins = &PINS_WS147;
    static const int SENSE[] = { 4, 5, 14, 15, 21, 22 };
#else
    const panel_t *panel = &WS147_C5;
    s_pins = &PINS_WS147_C5;
    static const int SENSE[] = { 4, 5, 6, 10, 23, 24 };
#endif
    char line[64], st[sizeof(SENSE)];
    int n = 0;
    for (size_t i = 0; i < sizeof(SENSE) / sizeof(SENSE[0]); i++) {
        st[i] = pin_state(SENSE[i]);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %d=%c", SENSE[i], st[i]);
    }
    // The board's signature without its panel's help (its data line is write-only, so the ID read gets nothing back):
    // the card slot's pull-ups on SD CS 4 and MISO 5, the backlight transistor's pull-down on 22, and the C6FH4
    // (QFN32, flash in the package) Waveshare builds it on. Bench 2026-11-05.
    uint32_t pkg = esp_efuse_get_pkg_ver();
#if CONFIG_IDF_TARGET_ESP32C6
    bool sig = st[0] == 'H' && st[1] == 'H' && st[5] == 'L' && pkg == 1;
#else
    // C5: the same slot pull-ups on 4 and 5 plus the card's DI pull-up on MOSI 6, while the backlight (10), panel CS
    // (23) and DC (24) float. Every other C5 screen board (AWOK Dual C5, Koko V8, Mini v3) has a pull-up on 23 and
    // never gets this far. Bench 2026-11-10.
    bool sig = st[0] == 'H' && st[1] == 'H' && st[2] == 'H' && st[3] == 'F' && st[4] == 'F' && st[5] == 'F';
#endif
    n += snprintf(line + n, sizeof(line) - (size_t)n, " pkg=%lu", (unsigned long)pkg);
    ESP_LOGI(TAG, "waveshare lcd probe: pins%s", line);
    snprintf(s_probe_note, sizeof(s_probe_note), "waveshare pins%s", line);
    if (!bus_open("waveshare lcd", false)) {
        strlcat(s_probe_note, " gate closed", sizeof(s_probe_note));
        s_pins = &PINS_C5;
        return false;
    }
    // The panel's reset line: a hardware reset, then the ID read. RDDID (04h) is one dummy clock, then 24 bits; RDID1
    // (DAh) one byte. The ST7789 answers on its one data line (3-wire on the bus's MOSI).
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RST, 1); esp_rom_delay_us(1000);
    gpio_set_level(PIN_RST, 0); esp_rom_delay_us(10000);
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t r[4] = {0};
    bool ok = false;
    if (add_device(CLK_READ_HZ / 2, true) == ESP_OK) {
        read_reg(0xDA, r, 1, true);
        uint8_t id1 = r[0];
        read_reg(0x04, r, 4, true);
        uint32_t raw = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
        uint32_t id = (raw >> 7) & 0xFFFFFF;
        ok = id1 == 0x85 || (id >> 16) == 0x85;   // an ST7789 that answers
        ESP_LOGI(TAG, "waveshare lcd probe: RDID1 %02x, RDDID raw %08lx (id %06lx)", id1, (unsigned long)raw, (unsigned long)id);
        size_t k = strlen(s_probe_note);
        snprintf(s_probe_note + k, sizeof(s_probe_note) - k, " id1=%02x rddid=%08lx", id1, (unsigned long)raw);
    }
    ESP_LOGI(TAG, "waveshare lcd probe: %s", ok ? "ST7789" : sig ? "board signature" :
             stored ? "no ID read, stored identity says Waveshare" : "none");
    {
        size_t k = strlen(s_probe_note);
        snprintf(s_probe_note + k, sizeof(s_probe_note) - k, " -> %s", ok ? "id" : sig ? "signature" : stored ? "stored" : "none");
    }
    ok = ok || sig;
    if ((!ok && !stored) || add_device(panel->hz, false) != ESP_OK) { bus_release(); s_pins = &PINS_C5; return false; }
    s_panel = panel;
    return true;
#endif
}

// ── Waveshare ESP32-S3-Touch-LCD-3.5: I2C fingerprint, the panel through esp_lcd, touch and battery ──
#if CONFIG_IDF_TARGET_ESP32S3
#define WS35_SDA 8
#define WS35_SCL 7
#define WS35_MOSI 1
#define WS35_SCLK 5
#define WS35_DC 3
#define FT6336_ADDR  0x38
#define AXP2101_ADDR 0x34
#define TCA9554_ADDR 0x20
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ft, s_axp, s_tca;

static esp_err_t i2c_rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *out, size_t n) {
    return i2c_master_transmit_receive(d, &reg, 1, out, n, 30);
}
static esp_err_t i2c_wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(d, b, 2, 30);
}
static i2c_master_dev_handle_t i2c_dev(uint8_t addr) {
    i2c_master_dev_handle_t d = NULL;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    return i2c_master_bus_add_device(s_i2c, &dc, &d) == ESP_OK ? d : NULL;
}
// Both lines read high against the internal pull-downs: the bus's own pull-ups. Nothing is clocked on a board without.
static bool ws35_i2c_pulled_up(void) {
    gpio_config_t io = { .pin_bit_mask = (1ULL << WS35_SDA) | (1ULL << WS35_SCL), .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(50);
    bool up = gpio_get_level(WS35_SDA) && gpio_get_level(WS35_SCL);
    gpio_reset_pin(WS35_SDA);
    gpio_reset_pin(WS35_SCL);
    return up;
}

// ── touch: tap (dismiss an alert), swipe left / right (pages), hold (stealth on / off) ──
#define TOUCH_PAGES 3
static void touch_task(void *arg) {
    (void)arg;
    bool down = false, held = false;
    int x0 = 0, y0 = 0, x = 0, y = 0;
    uint32_t t0 = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        uint8_t t[5];
        if (i2c_rd(s_ft, 0x02, t, 5) != ESP_OK) continue;
        int n = t[0] & 0x0F;
        uint32_t now = now_ms();
        if (n >= 1 && n <= 2) {
            x = ((t[1] & 0x0F) << 8) | t[2];
            y = ((t[3] & 0x0F) << 8) | t[4];
            if (!down) { down = true; held = false; x0 = x; y0 = y; t0 = now; }
            int mx = x - x0, my = y - y0;
            if (!held && now - t0 >= 1200 && mx * mx + my * my < 30 * 30) {   // a long press, still held: stealth
                held = true;
                if (s_touch_fn) s_touch_fn(LCD_TOUCH_STEALTH);
                else s_stealth = !s_stealth;
                if (s_task) xTaskNotifyGive(s_task);
            }
            continue;
        }
        if (!down) continue;
        down = false;
        if (held || s_stealth) continue;   // dark: only a long press does anything
        int dx = x - x0, dy = y - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        bool alerting = (int32_t)(s_alert_until - now) > 0;
        if (adx >= 60 && adx > 2 * ady) {   // a swipe: the next page leftward, the one before rightward
            if (!alerting) s_page = (uint8_t)((s_page + (dx < 0 ? 1 : TOUCH_PAGES - 1)) % TOUCH_PAGES);
        } else if (adx < 30 && ady < 30 && now - t0 < 800 && alerting) {   // a tap ends the alert
            if (s_touch_fn) s_touch_fn(LCD_TOUCH_DISMISS);
            else s_alert_until = now;
        }
        if (s_task) xTaskNotifyGive(s_task);
    }
}
#endif

bool lcd_probe_ws35(void) {
#if !CONFIG_IDF_TARGET_ESP32S3
    return false;
#else
    if (s_io) return s_panel == &WS35;
    if (!ws35_i2c_pulled_up()) {
        snprintf(s_probe_note, sizeof(s_probe_note), "ws35: I2C 8/7 not pulled up");
        ESP_LOGI(TAG, "ws35 probe: I2C on SDA %d / SCL %d not pulled up, not probed", WS35_SDA, WS35_SCL);
        return false;
    }
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = WS35_SDA, .scl_io_num = WS35_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7 };
    if (i2c_new_master_bus(&bc, &s_i2c) != ESP_OK) return false;
    uint8_t ft = 0, axp = 0, tca = 0;
    bool has_ft = i2c_master_probe(s_i2c, FT6336_ADDR, 30) == ESP_OK, has_axp = i2c_master_probe(s_i2c, AXP2101_ADDR, 30) == ESP_OK,
         has_tca = i2c_master_probe(s_i2c, TCA9554_ADDR, 30) == ESP_OK;
    if (has_ft) { s_ft = i2c_dev(FT6336_ADDR); if (!s_ft || i2c_rd(s_ft, 0xA3, &ft, 1) != ESP_OK) ft = 0; }
    if (has_axp) { s_axp = i2c_dev(AXP2101_ADDR); if (!s_axp || i2c_rd(s_axp, 0x03, &axp, 1) != ESP_OK) axp = 0; }
    if (has_tca) { s_tca = i2c_dev(TCA9554_ADDR); if (!s_tca || i2c_rd(s_tca, 0x03, &tca, 1) != ESP_OK) has_tca = false; }
    // FT6336U (chip id 0x64; FT6236 0x36), AXP2101 (chip id 0x4A) and a TCA9554 together: this board and no other.
    bool ok = (ft == 0x64 || ft == 0x36) && axp == 0x4A && has_tca;
    snprintf(s_probe_note, sizeof(s_probe_note), "ws35 i2c: ft6336 %02x  axp2101 %02x  tca9554 %s -> %s", ft, axp,
             has_tca ? "yes" : "no", ok ? "match" : "none");
    ESP_LOGI(TAG, "%s", s_probe_note);
    if (!ok) {
        if (s_ft) i2c_master_bus_rm_device(s_ft);
        if (s_axp) i2c_master_bus_rm_device(s_axp);
        if (s_tca) i2c_master_bus_rm_device(s_tca);
        s_ft = s_axp = s_tca = NULL;
        i2c_del_master_bus(s_i2c);
        s_i2c = NULL;
        gpio_reset_pin(WS35_SDA);
        gpio_reset_pin(WS35_SCL);
        return false;
    }
    // The panel's reset: TCA9554 pin 1, an output, low then high (as the factory firmware's io_expander_init).
    uint8_t cfg = 0xFF, out = 0xFF;
    i2c_rd(s_tca, 0x03, &cfg, 1);
    i2c_rd(s_tca, 0x01, &out, 1);
    i2c_wr(s_tca, 0x01, out | 0x02);
    i2c_wr(s_tca, 0x03, cfg & ~0x02);
    i2c_wr(s_tca, 0x01, out & ~0x02); vTaskDelay(pdMS_TO_TICKS(20));
    i2c_wr(s_tca, 0x01, out | 0x02);  vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t sb = { .mosi_io_num = WS35_MOSI, .miso_io_num = -1, .sclk_io_num = WS35_SCLK, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = LCD_W * SH * 2 + 16 };
    if (spi_bus_initialize(HOST, &sb, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    s_bus = true;
    esp_lcd_panel_io_spi_config_t ioc = { .cs_gpio_num = -1, .dc_gpio_num = WS35_DC, .spi_mode = 0,
                                          .pclk_hz = WS35.hz, .trans_queue_depth = 4, .lcd_cmd_bits = 8,
                                          .lcd_param_bits = 8, .on_color_trans_done = io_done_cb };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HOST, &ioc, &s_io) != ESP_OK) {
        spi_bus_free(HOST);
        s_bus = false;
        return false;
    }
    s_panel = &WS35;
    s_touch_ok = s_ft != NULL;
    if (s_touch_ok) xTaskCreate(touch_task, "wd_touch", 2560, NULL, 2, NULL);
    return true;
#endif
}

bool lcd_battery(int *pct, int *mv) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_axp) return false;
    uint8_t st = 0, p = 0, v[2] = {0};
    if (i2c_rd(s_axp, 0x00, &st, 1) != ESP_OK || !(st & 0x08)) return false;   // status 1, bit 3: a battery is there
    if (i2c_rd(s_axp, 0xA4, &p, 1) != ESP_OK) return false;                      // fuel gauge, percent
    if (pct) *pct = p > 100 ? 100 : p;
    if (mv && i2c_rd(s_axp, 0x34, v, 2) == ESP_OK) *mv = ((v[0] & 0x3F) << 8) | v[1];   // battery voltage, 1 mV a step
    return true;
#else
    (void)pct; (void)mv;
    return false;
#endif
}

void lcd_set_touch_handler(lcd_touch_fn fn) { s_touch_fn = fn; }

// ── AP sightings ──
void lcd_note_ap(const uint8_t *bssid, uint8_t channel) {
    (void)bssid;
    if (!s_task) return;
    int a = lcd_act_index(channel);
    if (a >= 0) s_act[a] = s_act[a] > 226 ? 250 : s_act[a] + 24;
}

// ── panel ──
static void window(int y0, int y1) {
    int x0 = s_panel->xofs, x1 = s_panel->xofs + s_panel->w - 1;
    y0 += s_panel->yofs; y1 += s_panel->yofs;
    const uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t ra[] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    cmd(0x2C, NULL, 0);
}

// `lock`: take the bus for each changed strip only, so the microSD card (an offline run, or the app reading runs off it)
// waits one strip at most, never a whole frame's drawing.
#if CONFIG_IDF_TARGET_ESP32S3
static void render_io(const lcd_frame_t *f) {
    int w = s_panel->w;
    for (int k = 0; k < s_panel->h / SH; k++) {
        int y0 = k * SH;
        uint16_t *buf = s_io_buf[k & 1];
        if (s_io_busy && s_io_inflight == buf) io_wait();   // never draw into the strip still being sent
        lcd_draw_strip(buf, y0, f);
        uint32_t h = 2166136261u;
        const uint32_t *px = (const uint32_t *)buf;
        for (int i = 0; i < w * SH / 2; i++) h = (h ^ px[i]) * 16777619u;
        if (h == s_hash[k]) continue;
        io_wait();   // the bus is free again
        int x0 = s_panel->xofs, x1 = x0 + w - 1, ya = y0 + s_panel->yofs, yb = ya + SH - 1;
        const uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
        const uint8_t ra[] = { (uint8_t)(ya >> 8), (uint8_t)ya, (uint8_t)(yb >> 8), (uint8_t)yb };
        cmd(0x2A, ca, 4);
        cmd(0x2B, ra, 4);
        s_io_busy = true;
        s_io_inflight = buf;
        if (esp_lcd_panel_io_tx_color(s_io, 0x2C, buf, (size_t)w * SH * 2) == ESP_OK) s_hash[k] = h;
        else s_io_busy = false;
    }
    io_wait();
}
#endif

static void render(const lcd_frame_t *f, bool lock) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_io) { render_io(f); return; }   // no card on its bus: nothing to share it with
#endif
    int w = s_panel->w;
    for (int k = 0; k < s_panel->h / SH; k++) {
        int y0 = k * SH;
        lcd_draw_strip(s_strip, y0, f);
        uint32_t h = 2166136261u;
        const uint32_t *px = (const uint32_t *)s_strip;
        for (int i = 0; i < w * SH / 2; i++) h = (h ^ px[i]) * 16777619u;
        if (h == s_hash[k]) continue;
        if (lock) lcd_bus_lock();
        window(y0, y0 + SH - 1);
        if (send(true, s_strip, (size_t)w * SH * 2) == ESP_OK) s_hash[k] = h;
        if (lock) lcd_bus_unlock();
    }
}

// One frame's worth of state, gathered once.
static void gather(lcd_frame_t *f, uint32_t now) {
    memset(f, 0, sizeof(*f));
    f->w = s_panel->w;
    f->h = s_panel->h;
    f->st.batt_pct = -1;
    if (s_fill) s_fill(&f->st);
    f->now = now;
    f->t0 = s_t0;
    f->live = f->st.aps_live;
    f->session = f->st.aps_session;
    f->run = f->st.aps_run;
    uint32_t then = s_session_at[(now / 1000 + 1) % 10];   // the oldest of the ten samples
    f->fresh = f->session > then ? f->session - then : 0;
    f->cur = lcd_act_index(f->st.channel);
    for (int i = 0; i < LCD_ACT_N; i++) f->act[i] = s_act[i];
    f->alerting = !f->st.ota && (int32_t)(s_alert_until - now) > 0;   // an update's screen outranks alerts
    f->alert_on = !(((now - s_alert_since) / ALERT_BLINK_MS) & 1);
    memcpy(f->alert, s_alert_label, sizeof(f->alert));
    f->alert_rgb = s_alert_rgb;
    f->alert_police = s_alert_police;
    f->board = s_board;
    f->version = s_version;
    f->page = s_page;
    f->touch = s_touch_ok;
}

// Marauder Mini v3 pin 5 (lcd_set_mini_pin5). The Mini v3 MK lights a solid white screen with pin 5 as a PWM backlight
// while Marauder drives its screen (field 2026-11-20): on some boards pin 5 may be the panel's reset line instead.
static int s_pin5 = 0;
static bool s_no_bl;   // pin 5 is the reset line: nothing to dim
void lcd_set_mini_pin5(int mode) { s_pin5 = mode; }

static void backlight(int duty) {
    if (s_no_bl) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void lcd_task(void *arg) {
    (void)arg;
    bool dark = false;
    int bl = -1;
    uint32_t last_decay = now_ms();
    lcd_frame_t f;
    for (;;) {
        uint32_t now = now_ms();
        if (s_stealth != dark) {
            dark = s_stealth;
            lcd_bus_lock();
            cmd(dark ? 0x28 : 0x29, NULL, 0);
            lcd_bus_unlock();
            if (dark) { backlight(0); bl = 0; }
            else memset(s_hash, 0, sizeof(s_hash));   // redraw everything on the way back
        }
        if (dark) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FRAME_MS)); continue; }

        gather(&f, now);
        if (now - last_decay >= 1000) {   // activity fades; a new second's slot starts at zero
            last_decay = now;
            for (int i = 0; i < LCD_ACT_N; i++) s_act[i] = (uint8_t)(s_act[i] * 3 / 4);
            s_session_at[(now / 1000) % 10] = f.session;
        }
        bool alerting = f.alerting;
        render(&f, true);
        if (bl != BL_ON) { backlight(BL_ON); bl = BL_ON; }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(alerting ? ALERT_BLINK_MS / 2 : FRAME_MS));   // a touch wakes it early
    }
}

void lcd_start(const char *board, const char *version, lcd_fill_fn fill_fn) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_dev && !s_io) return;
#else
    if (!s_dev) return;
#endif
    if (!s_panel || s_task) return;
    s_strip = heap_caps_malloc((size_t)s_panel->w * SH * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_io && s_strip) {
        s_io_buf[0] = s_strip;
        s_io_buf[1] = heap_caps_malloc((size_t)s_panel->w * SH * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_io_buf[1]) s_io_buf[1] = s_strip;   // one buffer: still works, only without the overlap
    }
#endif
    if (!s_strip) {
        ESP_LOGW(TAG, "lcd: no memory");
        return;
    }
    s_board = board; s_version = version; s_fill = fill_fn;
    if (!s_bus_lock) s_bus_lock = xSemaphoreCreateMutex();

    s_no_bl = s_panel == &MINI && s_pin5 == 1;
    if (s_no_bl) {
        // Pin 5 as the panel's reset line: one clean pulse, then held high for good.
        gpio_set_direction(s_panel->bl, GPIO_MODE_OUTPUT);
        gpio_set_level(s_panel->bl, 1); esp_rom_delay_us(1000);
        gpio_set_level(s_panel->bl, 0); esp_rom_delay_us(10000);
        gpio_set_level(s_panel->bl, 1); vTaskDelay(pdMS_TO_TICKS(120));
        ESP_LOGI(TAG, "lcd: Mini pin 5 as reset");
    } else {
        ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                                   .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
        bool invert = s_panel->bl_low && !(s_panel == &MINI && s_pin5 == 2);
        ledc_channel_config_t cc = { .gpio_num = s_panel->bl, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                                     .timer_sel = LEDC_TIMER_1, .duty = 0, .flags.output_invert = invert };
        if (ledc_timer_config(&tc) != ESP_OK || ledc_channel_config(&cc) != ESP_OK) ESP_LOGW(TAG, "lcd: backlight PWM failed");
    }

    lcd_bus_lock();
    for (size_t i = 0; i < s_panel->n_init; i++) {
        const init_t *in = &s_panel->init[i];
        if (cmd(in->c, in->p, in->n) != ESP_OK) { lcd_bus_unlock(); ESP_LOGW(TAG, "lcd didn't take its setup"); return; }
        if (in->ms) vTaskDelay(pdMS_TO_TICKS(in->ms));
    }
    s_t0 = now_ms();
    lcd_frame_t f;
    gather(&f, s_t0);
    render(&f, false);   // the splash is on the panel before it lights up (the bus is already held)
    cmd(0x29, NULL, 0);
    lcd_bus_unlock();
    backlight(BL_ON);
    if (xTaskCreate(lcd_task, "wd_lcd", 4096, NULL, 1, &s_task) != pdPASS) { s_task = NULL; return; }
    ESP_LOGI(TAG, "lcd: %s running", s_panel->name);
}

bool lcd_running(void) { return s_task != NULL; }

// ── microSD slot on the panel's bus (Koko V8 / AWOK Dual C5 / Mini v3: CS 10; Waveshare 1.47: CS 4) ──
bool lcd_sd_slot(int *host, int *cs) {
    // Waveshare 1.47: no GPS, so nothing would ever be saved; its card select is only held idle (bus_open).
    if (!s_task || !s_panel || is_ws147() || PIN_SD_CS < 0 || !s_sd_cs) return false;
    if (host) *host = HOST;
    if (cs) *cs = PIN_SD_CS;
    return true;
}

void lcd_bus_lock(void) { if (s_bus_lock) xSemaphoreTake(s_bus_lock, portMAX_DELAY); }
void lcd_bus_unlock(void) { if (s_bus_lock) xSemaphoreGive(s_bus_lock); }
const char *lcd_size(void) { return !s_task ? "" : s_panel->size; }

void lcd_alert(const char *label, uint32_t ms, bool soft, uint32_t rgb, bool police) {
    if (!s_task) return;
    uint32_t now = now_ms();
    if (!ms) { s_alert_until = now; return; }   // `led off`: back to the dashboard
    if (soft && (int32_t)(s_alert_until - now) > 0) return;
    size_t n = 0;
    for (; label[n] && n < sizeof(s_alert_label) - 1; n++)   // one word from the app: '_' stands for a space
        s_alert_label[n] = label[n] == '_' ? ' ' : (char)toupper((unsigned char)label[n]);
    s_alert_label[n] = '\0';
    if (!n) strcpy(s_alert_label, "ALERT");
    s_alert_rgb = rgb;
    s_alert_police = police;
    s_alert_since = now;
    s_alert_until = now + ms;
}

void lcd_set_stealth(bool on) { s_stealth = on; }
