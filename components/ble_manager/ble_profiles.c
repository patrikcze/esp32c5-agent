/* ble_profiles.c — decoders for standard GATT characteristics (portable). */
#include <stdint.h>

#include "ble_mgr.h"

bool ble_decode_known(uint16_t uuid16, const uint8_t *d, size_t n, double *value, const char **unit)
{
    switch (uuid16) {
    case 0x2A6E: /* Temperature: sint16, 0.01 degC; 0x8000 = unknown */
        if (n != 2) return false;
        if (d[0] == 0x00 && d[1] == 0x80) return false;
        *value = (int16_t)(d[0] | (d[1] << 8)) / 100.0;
        *unit = "C";
        return true;
    case 0x2A6F: /* Humidity: uint16, 0.01 %; 0xFFFF = unknown */
        if (n != 2 || (d[0] == 0xFF && d[1] == 0xFF)) return false;
        *value = (uint16_t)(d[0] | (d[1] << 8)) / 100.0;
        *unit = "%";
        return true;
    case 0x2A19: /* Battery Level: uint8 0..100 % */
        if (n != 1 || d[0] > 100) return false;
        *value = d[0];
        *unit = "%";
        return true;
    case 0x2A6D: /* Pressure: uint32, 0.1 Pa */
        if (n != 4) return false;
        *value = (uint32_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24)) / 10.0;
        *unit = "Pa";
        return true;
    default:
        return false;
    }
}
