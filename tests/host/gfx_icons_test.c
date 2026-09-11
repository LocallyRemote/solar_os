#include <assert.h>
#include <stdio.h>

#include "solar_os_gfx.h"

int main(void)
{
    solar_os_gfx_icon_t icon = SOLAR_OS_GFX_ICON_COUNT;

#define SOLAR_OS_GFX_ICON_ENTRY(symbol, name)                         \
    assert(solar_os_gfx_icon_from_name(name, &icon) == ESP_OK);      \
    assert(icon == SOLAR_OS_GFX_ICON_##symbol);
#include "solar_os_gfx_icon_table.inc"
#undef SOLAR_OS_GFX_ICON_ENTRY

    assert(solar_os_gfx_icon_from_name("tablet", &icon) == ESP_OK);
    assert(icon == SOLAR_OS_GFX_ICON_TABLET);
    assert(solar_os_gfx_icon_from_name("TABLET", &icon) == ESP_ERR_NOT_FOUND);
    assert(solar_os_gfx_icon_from_name("not-an-icon", &icon) == ESP_ERR_NOT_FOUND);
    assert(solar_os_gfx_icon_from_name(NULL, &icon) == ESP_ERR_INVALID_ARG);
    assert(solar_os_gfx_icon_from_name("tablet", NULL) == ESP_ERR_INVALID_ARG);

    puts("gfx icon tests: ok");
    return 0;
}
