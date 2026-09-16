// HDMI DDC (the display's EDID EEPROM at I2C 0x50) over the PiCowBell's GPIO4 (SDA) / GPIO5 (SCL).
// The board level-shifts these to the connector and pulls them up on both sides.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Reads 128-byte EDID block `block` (0 = base block, 1 = first extension) into out.
// Bit-banged open-drain at ~500 Hz: slow, but every step is checked and it never needs the I2C
// peripheral (whose timeout behaviour through the level shifters could not tell "no display"
// from "bus problem"). Takes ~1 s. Call from core 0 only.
bool ddc_read_edid(uint8_t block, uint8_t out[128]);
