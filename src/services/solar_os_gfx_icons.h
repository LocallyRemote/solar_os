#pragma once

/*
 * Stable SolarOS names for the complete Open Iconic glyph set.
 *
 * The names follow the upstream icon names, but callers must use these
 * identifiers instead of depending on the font's private glyph encoding.
 */
typedef enum {
#define SOLAR_OS_GFX_ICON_ENTRY(symbol, name) SOLAR_OS_GFX_ICON_##symbol,
#include "solar_os_gfx_icon_table.inc"
#undef SOLAR_OS_GFX_ICON_ENTRY
    SOLAR_OS_GFX_ICON_COUNT,
} solar_os_gfx_icon_t;

typedef enum {
    SOLAR_OS_GFX_ICON_SIZE_8 = 8,
    SOLAR_OS_GFX_ICON_SIZE_16 = 16,
    SOLAR_OS_GFX_ICON_SIZE_32 = 32,
    SOLAR_OS_GFX_ICON_SIZE_48 = 48,
    SOLAR_OS_GFX_ICON_SIZE_64 = 64,
} solar_os_gfx_icon_size_t;
