#pragma once

/* Bluedroid-only callback dispatch shared with the HID keyboard profile. */
#include "esp_gattc_api.h"

void solar_os_ble_bluedroid_gatt_event(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param);
