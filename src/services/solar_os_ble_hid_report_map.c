#include "solar_os_ble_hid_report_map.h"
#include <string.h>

bool solar_os_ble_hid_report_map(const uint8_t *data, size_t len, bool ids[256])
{
    struct globals { uint32_t page; uint8_t id; } global = {0}, stack[8];
    bool collections[16];
    size_t globals_depth = 0, depth = 0, offset = 0;
    uint32_t usage = 0, usage_page = 0;
    bool has_usage = false;
    if (!data || !len || !ids) return false;
    memset(ids, 0, 256 * sizeof(*ids));
    while (offset < len) {
        uint8_t prefix = data[offset++];
        if (prefix == 0xfe) return false; /* Long items are not used by HOGP keyboards. */
        size_t n = prefix & 3;
        if (n == 3) n = 4;
        if (n > len - offset) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < n; ++i) value |= (uint32_t)data[offset++] << (8 * i);
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: global.page = value; break;
            case 8:
                if (!value || value > 255) return false;
                global.id = value;
                break;
            case 10:
                if (n || globals_depth == 8) return false;
                stack[globals_depth++] = global;
                break;
            case 11:
                if (n || !globals_depth) return false;
                global = stack[--globals_depth];
                break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 0) {
                usage = value & 0xffff;
                usage_page = n == 4 ? value >> 16 : global.page;
                has_usage = true;
            }
            if (tag == 10) return false; /* Unsupported local delimiter sets. */
        } else if (type == 0) {
            switch (tag) {
            case 10: /* Collection */
                if (n != 1 || depth == 16) return false;
                collections[depth] = (depth && collections[depth - 1]) ||
                    (value == 1 && has_usage && usage_page == 1 && (usage == 6 || usage == 7));
                ++depth;
                break;
            case 12: /* End Collection */
                if (n || !depth) return false;
                --depth;
                break;
            case 8: /* Input */
                if (!n || !depth) return false;
                if (collections[depth - 1]) ids[global.id] = true;
                break;
            default: break;
            }
            has_usage = false;
        } else return false;
    }
    return depth == 0 && globals_depth == 0;
}
