#include "solar_os_virtual_display.h"

#include <string.h>

#include "solar_os_display.h"
#include "solar_os_memory.h"

#define VIRTUAL_TILE_WIDTH ((SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_WIDTH + 7U) / 8U)
#define VIRTUAL_TILE_HEIGHT ((SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_HEIGHT + 7U) / 8U)
#define VIRTUAL_BUFFER_SIZE (VIRTUAL_TILE_WIDTH * VIRTUAL_TILE_HEIGHT * 8U)

struct solar_os_virtual_display {
    u8g2_t u8g2;
    uint8_t *buffer;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    bool registered;
};

static const u8x8_display_info_t virtual_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .sck_clock_hz = 4000000UL,
    .i2c_bus_clock_100kHz = 4,
    .tile_width = VIRTUAL_TILE_WIDTH,
    .tile_height = VIRTUAL_TILE_HEIGHT,
    .pixel_width = SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_WIDTH,
    .pixel_height = SOLAR_OS_VIRTUAL_DISPLAY_NATIVE_HEIGHT,
};

static uint8_t virtual_display_cb(u8x8_t *u8x8,
                                  uint8_t msg,
                                  uint8_t arg_int,
                                  void *arg_ptr)
{
    (void)arg_int;
    (void)arg_ptr;

    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &virtual_display_info);
    }
    return 1;
}

esp_err_t solar_os_virtual_display_create(const char *name,
                                          solar_os_virtual_display_t **display)
{
    if (name == NULL || name[0] == '\0' || display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *display = NULL;

    solar_os_virtual_display_t *created =
        solar_os_memory_calloc(1,
                               sizeof(*created),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "display.virtual");
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }
    created->buffer =
        solar_os_memory_calloc(1,
                               VIRTUAL_BUFFER_SIZE,
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "display.virtual.fb");
    if (created->buffer == NULL) {
        solar_os_memory_free(created);
        return ESP_ERR_NO_MEM;
    }

    u8g2_SetupDisplay(&created->u8g2,
                      virtual_display_cb,
                      u8x8_cad_empty,
                      u8x8_dummy_cb,
                      u8x8_dummy_cb);
    u8g2_SetupBuffer(&created->u8g2,
                     created->buffer,
                     VIRTUAL_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     U8G2_R1);
    u8g2_InitDisplay(&created->u8g2);
    u8g2_ClearBuffer(&created->u8g2);

    solar_os_display_target_t target = {0};
    strlcpy(target.name, name, sizeof(target.name));
    strlcpy(target.source, "virtual", sizeof(target.source));
    strlcpy(target.driver, "framebuffer", sizeof(target.driver));
    strlcpy(target.role, "remote", sizeof(target.role));
    target.width = u8g2_GetDisplayWidth(&created->u8g2);
    target.height = u8g2_GetDisplayHeight(&created->u8g2);
    target.ready = true;
    target.black_is_one = false;
    target.u8g2 = &created->u8g2;

    const esp_err_t err = solar_os_display_register_target(&target);
    if (err != ESP_OK) {
        solar_os_memory_free(created->buffer);
        solar_os_memory_free(created);
        return err;
    }

    strlcpy(created->name, target.name, sizeof(created->name));
    created->registered = true;
    *display = created;
    return ESP_OK;
}

esp_err_t solar_os_virtual_display_destroy(solar_os_virtual_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display->registered) {
        const esp_err_t err = solar_os_display_unregister_target(display->name);
        if (err != ESP_OK) {
            return err;
        }
        display->registered = false;
    }

    solar_os_memory_free(display->buffer);
    display->buffer = NULL;
    solar_os_memory_free(display);
    return ESP_OK;
}

u8g2_t *solar_os_virtual_display_u8g2(solar_os_virtual_display_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}
