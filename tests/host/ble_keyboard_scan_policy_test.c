#include <assert.h>
#include <stdio.h>

#include "solar_os_ble_keyboard_scan_policy.h"

int main(void)
{
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("BLE Keyboard"));
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("KEYCHRON K3"));
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("tiny-kbd"));
    assert(!solar_os_ble_keyboard_scan_name_is_keyboard_like("mouse"));
    assert(!solar_os_ble_keyboard_scan_name_is_keyboard_like(NULL));

    const uint8_t remembered_bda[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t same_bda[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t unrelated_bda[6] = {6, 5, 4, 3, 2, 1};
    assert(solar_os_ble_keyboard_scan_reconnect_bda_matches(
        remembered_bda, same_bda));
    assert(!solar_os_ble_keyboard_scan_reconnect_bda_matches(
        remembered_bda, unrelated_bda));
    assert(!solar_os_ble_keyboard_scan_reconnect_bda_matches(
        remembered_bda, NULL));
    assert(!solar_os_ble_keyboard_scan_reconnect_bda_matches(
        NULL, same_bda));

    assert(solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0x00U));
    assert(solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0x01U));
    assert(!solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0x02U));
    assert(!solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0x03U));
    assert(!solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0x04U));
    assert(!solar_os_ble_keyboard_scan_reconnect_event_is_connectable(0xffU));

    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        false, false, false, 0, false, false, -20));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        false, false, false, 0, true, false, -80));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        false, true, false, -20, false, true, -90));
    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        false, true, true, -90, true, false, -10));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        false, true, true, -70, true, true, -60));
    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        false, true, true, -60, true, true, -60));
    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        true, true, true, -90, true, true, -20));

    puts("BLE keyboard scan policy tests: ok");
    return 0;
}
