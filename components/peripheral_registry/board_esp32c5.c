/*
 * board_esp32c5.c — pin table for ESP32-C5-DevKitC-1 (ESP32-C5-WROOM-1).
 *
 * Sources (verified 2026-11-01):
 *  - ESP-IDF v5.5.6 soc/esp32c5: SOC_GPIO_PIN_COUNT=29, ADC1_CH0..5 = GPIO1..6,
 *    U0TXD=GPIO11, U0RXD=GPIO12, MSPI pins GPIO15..22
 *  - ESP-IDF GPIO docs (esp32c5): strapping GPIO2/7/25/27/28; GPIO16..22 flash/PSRAM;
 *    GPIO13/14 USB-Serial-JTAG
 *  - ESP32-C5-DevKitC-1 user guide: RGB LED on GPIO27; GPIO15 unavailable on
 *    modules with PSRAM; MTDI (GPIO3) and GPIO26 listed as strapping
 * GPIO28 is the boot strapping pin wired to the BOOT button on the DevKitC-1.
 * Verify against your board revision before wiring loads.
 */
#include "periph.h"

#define IO (PCAP_IN | PCAP_OUT)
#define IOA (PCAP_IN | PCAP_OUT | PCAP_ADC)

static const board_pin_t k_pins[BOARD_GPIO_COUNT] = {
    [0] = {IO, PIN_CAUTION, "XTAL_32K_P (free if no 32k crystal)"},
    [1] = {IOA, PIN_CAUTION, "XTAL_32K_N, ADC1_CH0"},
    [2] = {IOA, PIN_STRAP, "strapping (MTMS), ADC1_CH1"},
    [3] = {IOA, PIN_STRAP, "strapping (MTDI), ADC1_CH2"},
    [4] = {IOA, PIN_FREE, "ADC1_CH3 (MTCK)"},
    [5] = {IOA, PIN_FREE, "ADC1_CH4 (MTDO)"},
    [6] = {IOA, PIN_FREE, "ADC1_CH5"},
    [7] = {IO, PIN_STRAP, "strapping"},
    [8] = {IO, PIN_FREE, ""},
    [9] = {IO, PIN_FREE, ""},
    [10] = {IO, PIN_FREE, ""},
    [11] = {IO, PIN_RESERVED, "UART0 TX (console)"},
    [12] = {IO, PIN_RESERVED, "UART0 RX (console)"},
    [13] = {IO, PIN_RESERVED, "USB-Serial-JTAG D-"},
    [14] = {IO, PIN_RESERVED, "USB-Serial-JTAG D+"},
    [15] = {IO, PIN_RESERVED, "PSRAM CS (WROOM-1 with PSRAM)"},
    [16] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [17] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [18] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [19] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [20] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [21] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [22] = {IO, PIN_RESERVED, "SPI flash/PSRAM"},
    [23] = {IO, PIN_FREE, ""},
    [24] = {IO, PIN_FREE, ""},
    [25] = {IO, PIN_STRAP, "strapping"},
    [26] = {IO, PIN_STRAP, "strapping"},
    [27] = {IO, PIN_STRAP, "strapping, on-board RGB LED"},
    [28] = {IO, PIN_STRAP, "strapping, BOOT button"},
};

const board_t board_esp32c5_devkitc1 = {
    .name = "esp32c5-devkitc-1",
    .pins = k_pins,
    .n_pins = BOARD_GPIO_COUNT,
    .rgb_led_pin = 27,
    .boot_btn_pin = 28,
};
