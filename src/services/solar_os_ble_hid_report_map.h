#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Classify input report IDs belonging to Keyboard/Keypad Application
 * collections. This does not reinterpret SolarOS's existing key reports. */
bool solar_os_ble_hid_report_map(const uint8_t *data, size_t len, bool keyboard_ids[256]);
