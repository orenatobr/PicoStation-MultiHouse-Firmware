/* PicoStation board diagnostic: runs on the Pico powered by USB alone (console unplugged) and prints over USB serial.
 * Checks, with the pin map of the PicoStation board (boards/picostation_pico1.cmake):
 *   1. the SD card on SPI1 (GP8 MISO, GP9 CS, GP10 SCK, GP11 MOSI): init, capacity class, blocks 0 and 5 (V2 headers A and B)
 *   2. the Si5351 clock chip on I2C0 (GP12 SDA, GP13 SCL): address scan and status register 0
 *   3. the level of every other PicoStation signal pin with the internal pulls disabled, then pulled up and pulled down
 * Read the output with:  screen /dev/cu.usbmodem* 115200   (or any serial terminal)  */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"

#define SD_SPI spi1
#define PIN_MISO 8
#define PIN_CS 9
#define PIN_SCK 10
#define PIN_MOSI 11
#define PIN_SDA 12
#define PIN_SCL 13

static void cs(int v) { gpio_put(PIN_CS, v); }

static uint8_t xfer(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    xfer(0xFF);
    xfer(0x40 | cmd);
    xfer(arg >> 24); xfer(arg >> 16); xfer(arg >> 8); xfer(arg);
    xfer(crc);
    uint8_t r = 0xFF;
    for (int i = 0; i < 16 && (r & 0x80); ++i) r = xfer(0xFF);
    return r;
}

static int sdhc;

static int sd_init(void) {
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CS); gpio_set_dir(PIN_CS, GPIO_OUT); cs(1);
    for (int i = 0; i < 10; ++i) xfer(0xFF);
    cs(0);
    uint8_t r = sd_cmd(0, 0, 0x95);
    printf("  CMD0 -> 0x%02x (expect 0x01)\n", r);
    if (r != 0x01) { cs(1); return -1; }
    r = sd_cmd(8, 0x1AA, 0x87);
    uint8_t ocr[4] = {xfer(0xFF), xfer(0xFF), xfer(0xFF), xfer(0xFF)};
    printf("  CMD8 -> 0x%02x, echo %02x %02x %02x %02x\n", r, ocr[0], ocr[1], ocr[2], ocr[3]);
    for (int i = 0; i < 2000; ++i) {
        sd_cmd(55, 0, 0x65);
        r = sd_cmd(41, 0x40000000, 0x77);
        if (r == 0) break;
        sleep_ms(1);
    }
    printf("  ACMD41 -> 0x%02x (expect 0x00)\n", r);
    if (r != 0) { cs(1); return -2; }
    r = sd_cmd(58, 0, 0xFD);
    uint8_t o[4] = {xfer(0xFF), xfer(0xFF), xfer(0xFF), xfer(0xFF)};
    sdhc = (o[0] & 0x40) != 0;
    printf("  CMD58 -> 0x%02x OCR %02x %02x %02x %02x (%s)\n", r, o[0], o[1], o[2], o[3], sdhc ? "SDHC/SDXC, block addressing" : "SDSC, byte addressing");
    cs(1);
    spi_set_baudrate(SD_SPI, 12 * 1000 * 1000);
    return 0;
}

static int sd_read(uint32_t lba, uint8_t *buf) {
    cs(0);
    uint8_t r = sd_cmd(17, sdhc ? lba : lba * 512, 0xFF);
    if (r != 0) { cs(1); return -1; }
    uint8_t t = 0xFF;
    for (int i = 0; i < 100000 && t == 0xFF; ++i) t = xfer(0xFF);
    if (t != 0xFE) { cs(1); return -2; }
    spi_read_blocking(SD_SPI, 0xFF, buf, 512);
    xfer(0xFF); xfer(0xFF);
    cs(1);
    return 0;
}

static void show_header(const char *name, uint32_t lba) {
    static uint8_t b[512];
    int r = sd_read(lba, b);
    if (r) { printf("  block %u (%s): read failed (%d)\n", lba, name, r); return; }
    int magic = memcmp(b, "PSXRAW", 6) == 0;
    printf("  block %u (%s): %s, version %u, flags 0x%02x, cases %u, discs %u\n", lba, name,
           magic ? "magic PSXRAW ok" : "NO PSXRAW magic", b[6], b[7], b[8] | b[9] << 8, b[10] | b[11] << 8);
    printf("    first 32 bytes:");
    for (int i = 0; i < 32; ++i) printf(" %02x", b[i]);
    printf("\n");
}

static void i2c_scan(void) {
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); gpio_pull_up(PIN_SCL);
    int found = 0;
    for (int a = 0x08; a < 0x78; ++a) {
        uint8_t d;
        if (i2c_read_timeout_us(i2c0, a, &d, 1, false, 5000) == 1) { printf("  device at 0x%02x\n", a); ++found; }
    }
    if (!found) printf("  no I2C device answered (Si5351 expected at 0x60)\n");
    uint8_t reg = 0, v = 0xFF;
    if (i2c_write_timeout_us(i2c0, 0x60, &reg, 1, true, 5000) == 1 && i2c_read_timeout_us(i2c0, 0x60, &v, 1, false, 5000) == 1)
        printf("  Si5351 register 0 (status) = 0x%02x (bit7 SYS_INIT %s)\n", v, (v & 0x80) ? "set (still initializing)" : "clear");
}

static const uint8_t SIGNALS[] = {0, 1, 2, 4, 6, 7, 14, 15, 16, 17, 18, 19, 21, 25, 26, 27};

static void pins(void) {
    printf("  pin: floating / pull-up / pull-down\n");
    for (unsigned i = 0; i < sizeof SIGNALS; ++i) {
        uint p = SIGNALS[i];
        gpio_init(p); gpio_set_dir(p, GPIO_IN);
        gpio_disable_pulls(p); sleep_ms(1); int f = gpio_get(p);
        gpio_pull_up(p); sleep_ms(1); int u = gpio_get(p);
        gpio_pull_down(p); sleep_ms(1); int d = gpio_get(p);
        gpio_disable_pulls(p);
        printf("  GP%-2u: %d / %d / %d%s\n", p, f, u, d, (u == 0 && d == 1) ? "  (driven both ways?)" : (u == 1 && d == 0 && f == 0 ? "  (follows the pull: not driven)" : ""));
    }
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 100 && !stdio_usb_connected(); ++i) sleep_ms(100);
    sleep_ms(500);
    for (int round = 1;; ++round) {
        printf("\n===== PicoStation diagnostic, round %d =====\n", round);
        printf("[SD card on SPI1]\n");
        if (sd_init() == 0) { show_header("header A", 0); show_header("header B", 5); }
        else printf("  SD card did not initialize: check the card, its contacts and the board wiring\n");
        printf("[Si5351 on I2C0]\n");
        i2c_scan();
        printf("[signal pins]\n");
        pins();
        sleep_ms(5000);
    }
}
