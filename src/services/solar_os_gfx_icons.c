#include "solar_os_gfx.h"

#include <stddef.h>
#include <string.h>

static const char *const icon_names[] = {
#define SOLAR_OS_GFX_ICON_ENTRY(symbol, name) [SOLAR_OS_GFX_ICON_##symbol] = name,
#include "solar_os_gfx_icon_table.inc"
#undef SOLAR_OS_GFX_ICON_ENTRY
};

_Static_assert(sizeof(icon_names) / sizeof(icon_names[0]) ==
                   SOLAR_OS_GFX_ICON_COUNT,
               "icon name table must match the icon enum");

esp_err_t solar_os_gfx_icon_from_name(const char *name,
                                      solar_os_gfx_icon_t *icon)
{
    if (name == NULL || name[0] == '\0' || icon == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < SOLAR_OS_GFX_ICON_COUNT; i++) {
        if (strcmp(name, icon_names[i]) == 0) {
            *icon = (solar_os_gfx_icon_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
