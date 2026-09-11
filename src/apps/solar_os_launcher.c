#include "solar_os_launcher.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_app_registry.h"
#include "solar_os_gfx.h"
#include "solar_os_json.h"
#include "solar_os_keys.h"
#include "solar_os_launcher_layout.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_launch.h"
#include "solar_os_shell_parse.h"
#include "solar_os_storage.h"

#define LAUNCHER_CONFIG_FILE "launcher.json"
#define LAUNCHER_CONFIG_MAX_BYTES (16U * 1024U)
#define LAUNCHER_GRID_MAX 8U
#define LAUNCHER_ITEM_MAX 32U
#define LAUNCHER_NAME_MAX 48U
#define LAUNCHER_ICON_NAME_MAX 32U
#define LAUNCHER_COMMAND_MAX 192U
#define LAUNCHER_TICK_MS 40U

typedef struct {
    solar_os_launcher_cell_t cell;
    solar_os_gfx_icon_t icon;
    char name[LAUNCHER_NAME_MAX];
    char command[LAUNCHER_COMMAND_MAX];
} launcher_item_t;

typedef struct {
    uint8_t columns;
    uint8_t rows;
    size_t item_count;
    launcher_item_t items[LAUNCHER_ITEM_MAX];
} launcher_config_t;

typedef struct {
    launcher_config_t config;
    size_t selected;
    int pointer_x;
    int pointer_y;
    bool pointer_relative;
    bool suspended;
    bool render_pending;
    char config_path[SOLAR_OS_STORAGE_PATH_MAX];
} launcher_state_t;

static void *launcher_state_storage;
#define launcher (*(launcher_state_t *)launcher_state_storage)

static const char launcher_default_config[] =
    "{\n"
    "  \"layout\": {\"columns\": 3, \"rows\": 2},\n"
    "  \"items\": [\n"
    "    {\"name\": \"Files\", \"icon\": \"folder\", \"command\": \"files\", \"column\": 0, \"row\": 0},\n"
    "    {\"name\": \"Manual\", \"icon\": \"book\", \"command\": \"help\", \"column\": 1, \"row\": 0},\n"
    "    {\"name\": \"Wi-Fi\", \"icon\": \"wifi\", \"command\": \"wifi\", \"column\": 2, \"row\": 0},\n"
    "    {\"name\": \"Clock\", \"icon\": \"clock\", \"command\": \"clock\", \"column\": 0, \"row\": 1},\n"
    "    {\"name\": \"Calculator\", \"icon\": \"calculator\", \"command\": \"calc\", \"column\": 1, \"row\": 1},\n"
    "    {\"name\": \"Writer\", \"icon\": \"pencil\", \"command\": \"writer\", \"column\": 2, \"row\": 1}\n"
    "  ]\n"
    "}\n";

static void launcher_copy_cells(solar_os_launcher_cell_t *cells)
{
    for (size_t i = 0U; i < launcher.config.item_count; i++) {
        cells[i] = launcher.config.items[i].cell;
    }
}

static bool launcher_text_valid(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; cursor++) {
        if (*cursor < 0x20U || *cursor == 0x7fU) {
            return false;
        }
    }
    return true;
}

static esp_err_t launcher_parse_icon(const solar_os_json_value_t *item,
                                     solar_os_gfx_icon_t *icon)
{
    if (item == NULL || icon == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_json_value_t *value =
        solar_os_json_object_get(item, "icon");
    if (solar_os_json_is_string(value)) {
        char name[LAUNCHER_ICON_NAME_MAX];
        esp_err_t err = solar_os_json_get_string(value, name, sizeof(name));
        return err == ESP_OK ? solar_os_gfx_icon_from_name(name, icon) : err;
    }
    if (solar_os_json_is_number(value)) {
        uint32_t numeric = 0U;
        esp_err_t err = solar_os_json_get_uint32(value, &numeric);
        if (err == ESP_OK && numeric >= SOLAR_OS_GFX_ICON_COUNT) {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            *icon = (solar_os_gfx_icon_t)numeric;
        }
        return err;
    }
    return ESP_ERR_INVALID_ARG;
}

static bool launcher_config_fits(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && launcher.config.columns != 0U &&
        launcher.config.rows != 0U &&
        solar_os_gfx_width(gfx) / launcher.config.columns >= 10U &&
        solar_os_gfx_height(gfx) / launcher.config.rows >= 10U;
}

static solar_os_gfx_icon_size_t launcher_icon_size(int available, int maximum)
{
    if (maximum >= 64 && available >= 64) return SOLAR_OS_GFX_ICON_SIZE_64;
    if (maximum >= 48 && available >= 48) return SOLAR_OS_GFX_ICON_SIZE_48;
    if (maximum >= 32 && available >= 32) return SOLAR_OS_GFX_ICON_SIZE_32;
    if (maximum >= 16 && available >= 16) return SOLAR_OS_GFX_ICON_SIZE_16;
    return SOLAR_OS_GFX_ICON_SIZE_8;
}

static void launcher_fit_title(solar_os_gfx_t *gfx,
                               const char *source,
                               int maximum_width,
                               char *title,
                               size_t title_size)
{
    strlcpy(title, source, title_size);
    size_t length = strlen(title);
    while (length > 0U &&
           solar_os_gfx_text_width(gfx, title) > (size_t)maximum_width) {
        do {
            length--;
        } while (length > 0U &&
                 ((unsigned char)title[length] & 0xc0U) == 0x80U);
        title[length] = '\0';
    }
}

static void launcher_cell_bounds(size_t item,
                                 int width,
                                 int height,
                                 int *x0,
                                 int *y0,
                                 int *x1,
                                 int *y1)
{
    const solar_os_launcher_cell_t *cell = &launcher.config.items[item].cell;
    *x0 = (int)cell->column * width / launcher.config.columns;
    *x1 = ((int)cell->column + 1) * width / launcher.config.columns;
    *y0 = (int)cell->row * height / launcher.config.rows;
    *y1 = ((int)cell->row + 1) * height / launcher.config.rows;
}

static void launcher_render(solar_os_context_t *ctx)
{
    if (launcher.suspended) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
    solar_os_gfx_line(gfx, 0, 0, width - 1, 0);
    solar_os_gfx_line(gfx, 0, height - 1, width - 1, height - 1);
    solar_os_gfx_line(gfx, 0, 0, 0, height - 1);
    solar_os_gfx_line(gfx, width - 1, 0, width - 1, height - 1);

    for (uint8_t column = 1U; column < launcher.config.columns; column++) {
        const int x = (int)column * width / launcher.config.columns;
        solar_os_gfx_line(gfx, x, 0, x, height - 1);
    }
    for (uint8_t row = 1U; row < launcher.config.rows; row++) {
        const int y = (int)row * height / launcher.config.rows;
        solar_os_gfx_line(gfx, 0, y, width - 1, y);
    }
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);

    for (size_t i = 0U; i < launcher.config.item_count; i++) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        launcher_cell_bounds(i, width, height, &x0, &y0, &x1, &y1);
        const int cell_width = x1 - x0;
        const int cell_height = y1 - y0;
        const bool selected = i == launcher.selected;
        if (selected && cell_width > 5 && cell_height > 5) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_fill_rect(gfx, x0 + 2, y0 + 2,
                                   cell_width - 4, cell_height - 4);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        const bool show_title = cell_width >= 24 && cell_height >= 24;
        const bool large_title = selected && cell_height >= 56;
        const int title_height = show_title ? (large_title ? 14 : 12) : 0;
        const int title_gap = show_title ? 3 : 0;
        int available = cell_width - 10;
        if (cell_height - title_height - title_gap - 6 < available) {
            available = cell_height - title_height - title_gap - 6;
        }
        const solar_os_gfx_icon_size_t icon_size =
            launcher_icon_size(available, selected ? 64 : 32);
        const int icon_pixels = (int)icon_size;
        const int icon_x = x0 + (cell_width - icon_pixels) / 2;
        const int group_height = icon_pixels + title_gap + title_height;
        int icon_y = y0 + (cell_height - group_height) / 2;
        if (icon_y < y0 + 1) {
            icon_y = y0 + 1;
        }
        solar_os_gfx_icon(gfx, icon_x, icon_y,
                          launcher.config.items[i].icon, icon_size);

        if (show_title) {
            solar_os_gfx_set_font(gfx, large_title ?
                SOLAR_OS_GFX_FONT_BOLD_14 : SOLAR_OS_GFX_FONT_SMALL);
            char title[LAUNCHER_NAME_MAX];
            launcher_fit_title(gfx, launcher.config.items[i].name,
                               cell_width - 6, title, sizeof(title));
            const int title_width = (int)solar_os_gfx_text_width(gfx, title);
            const int title_x = x0 + (cell_width - title_width) / 2;
            const int title_descent = large_title ? 3 : 2;
            const int title_y = icon_y + icon_pixels + title_gap +
                title_height - title_descent;
            solar_os_gfx_text(gfx, title_x, title_y, title);
        }
    }

    if (launcher.pointer_relative) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_circle(gfx, launcher.pointer_x, launcher.pointer_y, 3);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, launcher.pointer_x, launcher.pointer_y, 4);
    }
    solar_os_gfx_present(gfx);
    launcher.render_pending = false;
}

static esp_err_t launcher_parse_config(const char *source, size_t source_len)
{
    solar_os_json_doc_t *document = NULL;
    esp_err_t err = solar_os_json_parse(source, source_len, &document);
    const solar_os_json_value_t *root =
        err == ESP_OK ? solar_os_json_root(document) : NULL;
    const solar_os_json_value_t *layout =
        root != NULL ? solar_os_json_object_get(root, "layout") : NULL;
    const solar_os_json_value_t *items =
        root != NULL ? solar_os_json_object_get(root, "items") : NULL;
    uint32_t columns = 0U;
    uint32_t rows = 0U;
    if (err == ESP_OK &&
        (!solar_os_json_is_object(root) ||
         !solar_os_json_is_object(layout) ||
         !solar_os_json_is_array(items))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(layout, "columns", &columns);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(layout, "rows", &rows);
    }
    const size_t item_count = err == ESP_OK ? solar_os_json_array_size(items) : 0U;
    if (err == ESP_OK &&
        (columns == 0U || columns > LAUNCHER_GRID_MAX ||
         rows == 0U || rows > LAUNCHER_GRID_MAX ||
         item_count == 0U || item_count > LAUNCHER_ITEM_MAX)) {
        err = ESP_ERR_INVALID_SIZE;
    }

    memset(&launcher.config, 0, sizeof(launcher.config));
    launcher.config.columns = (uint8_t)columns;
    launcher.config.rows = (uint8_t)rows;
    launcher.config.item_count = item_count;
    for (size_t i = 0U; err == ESP_OK && i < item_count; i++) {
        const solar_os_json_value_t *value = solar_os_json_array_get(items, i);
        launcher_item_t *item = &launcher.config.items[i];
        solar_os_gfx_icon_t icon = SOLAR_OS_GFX_ICON_ACCOUNT_LOGIN;
        uint32_t column = 0U;
        uint32_t row = 0U;
        if (!solar_os_json_is_object(value)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = solar_os_json_get_path_string(value, "name",
                                            item->name, sizeof(item->name));
        if (err == ESP_OK) {
            err = launcher_parse_icon(value, &icon);
        }
        if (err == ESP_OK) {
            err = solar_os_json_get_path_string(value, "command",
                                                item->command,
                                                sizeof(item->command));
        }
        if (err == ESP_OK) {
            err = solar_os_json_get_path_uint32(value, "column", &column);
        }
        if (err == ESP_OK) {
            err = solar_os_json_get_path_uint32(value, "row", &row);
        }
        if (err == ESP_OK &&
            (!launcher_text_valid(item->name) ||
             !launcher_text_valid(item->command) ||
             column >= columns || row >= rows)) {
            err = ESP_ERR_INVALID_ARG;
        }
        item->icon = icon;
        item->cell.column = (uint8_t)column;
        item->cell.row = (uint8_t)row;
    }
    if (err == ESP_OK) {
        solar_os_launcher_cell_t cells[LAUNCHER_ITEM_MAX];
        launcher_copy_cells(cells);
        if (!solar_os_launcher_layout_valid(cells, item_count,
                                            (uint8_t)columns,
                                            (uint8_t)rows)) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    solar_os_json_free(document);
    return err;
}

static esp_err_t launcher_write_default_config(const char *path)
{
    char temporary[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_sibling_path(
        path, ".tmp", temporary, sizeof(temporary));
    if (err == ESP_OK) {
        err = solar_os_storage_sibling_path(
            path, ".bak", backup, sizeof(backup));
    }
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_storage_remove(temporary);
    const size_t length = sizeof(launcher_default_config) - 1U;
    char *staged = solar_os_memory_alloc(sizeof(launcher_default_config),
                                         SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
                                         "launcher.default");
    if (staged == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Keep both sides of flash filesystem I/O in internal RAM. The embedded
    // default itself is DROM-backed, and some tasks may use external stacks.
    memcpy(staged, launcher_default_config, length);
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        solar_os_memory_free(staged);
        return ESP_FAIL;
    }
    err = fwrite(staged, 1U, length, file) == length ?
        solar_os_storage_sync_file(file) : ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        size_t verify_len = 0U;
        err = solar_os_storage_read_file(temporary, staged,
                                         sizeof(launcher_default_config),
                                         &verify_len);
        if (err == ESP_OK &&
            (verify_len != length ||
             memcmp(staged, launcher_default_config, length) != 0)) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(temporary, path, backup);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(temporary);
    }
    solar_os_memory_free(staged);
    return err;
}

static esp_err_t launcher_load_config(void)
{
    struct stat info;
    if (stat(launcher.config_path, &info) != 0) {
        if (errno != ENOENT) {
            return ESP_FAIL;
        }
        esp_err_t err = launcher_parse_config(
            launcher_default_config, strlen(launcher_default_config));
        if (err == ESP_OK) {
            err = launcher_write_default_config(launcher.config_path);
        }
        if (err != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }
    if (info.st_size <= 0 || (uint64_t)info.st_size > LAUNCHER_CONFIG_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t source_len = (size_t)info.st_size;
    char *source = solar_os_memory_alloc(source_len + 1U,
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "launcher.config");
    if (source == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(launcher.config_path, "rb");
    if (file == NULL) {
        solar_os_memory_free(source);
        return ESP_FAIL;
    }
    const bool loaded = fread(source, source_len, 1U, file) == 1U &&
        fgetc(file) == EOF;
    fclose(file);
    if (!loaded) {
        solar_os_memory_free(source);
        return ESP_FAIL;
    }
    source[source_len] = '\0';
    const esp_err_t err = launcher_parse_config(source, source_len);
    solar_os_memory_free(source);
    if (err == ESP_OK && launcher.selected >= launcher.config.item_count) {
        launcher.selected = 0U;
    }
    return err;
}

static void launcher_finish_error(solar_os_context_t *ctx,
                                  const char *operation,
                                  esp_err_t err)
{
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    snprintf(message, sizeof(message), "launcher: %s: %s",
             operation, esp_err_to_name(err));
    solar_os_context_finish(ctx, 1, message);
}

static esp_err_t launcher_make_requested_app_child(solar_os_context_t *ctx)
{
    if (ctx->requested_app == NULL) {
        return ESP_OK;
    }
    const solar_os_app_t *requested_app = ctx->requested_app;
    const int argc = ctx->argc;
    char argv_storage[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char *argv[SOLAR_OS_APP_ARG_MAX] = {0};
    for (int i = 0; i < argc; i++) {
        strlcpy(argv_storage[i], ctx->argv[i], sizeof(argv_storage[i]));
        argv[i] = argv_storage[i];
    }
    return solar_os_context_request_launch_ex(
        ctx, requested_app, argc, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
}

static esp_err_t launcher_launch_script(solar_os_context_t *ctx,
                                        const char *argument)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_shell_resolve_path(ctx, argument,
                                                path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_shell_run_script(ctx, path, argument, true);
    return launcher_make_requested_app_child(ctx);
}

static esp_err_t launcher_launch_app(solar_os_context_t *ctx,
                                     const solar_os_app_registry_entry_t *entry,
                                     int argc,
                                     char **argv)
{
    if (entry == NULL || entry->app == NULL || argc < entry->min_argc ||
        (entry->max_argc != 0U && argc > entry->max_argc)) {
        return ESP_ERR_INVALID_ARG;
    }

    char resolved_path[SOLAR_OS_STORAGE_PATH_MAX];
    char *launch_argv[SOLAR_OS_APP_ARG_MAX] = {0};
    for (int i = 0; i < argc; i++) {
        if (strlen(argv[i]) >= SOLAR_OS_APP_ARG_LEN) {
            return ESP_ERR_INVALID_SIZE;
        }
        launch_argv[i] = argv[i];
    }
    const int path_arg = solar_os_shell_launch_path_arg(entry->name, argc, argv);
    if (path_arg >= 0) {
        const esp_err_t err = solar_os_shell_resolve_path(
            ctx, argv[path_arg], resolved_path, sizeof(resolved_path));
        if (err != ESP_OK) {
            return err;
        }
        launch_argv[path_arg] = resolved_path;
    }
    return solar_os_context_request_launch_ex(ctx, entry->app, argc, launch_argv,
                                              SOLAR_OS_LAUNCH_CHILD_RETURN);
}

static void launcher_activate(solar_os_context_t *ctx)
{
    if (launcher.selected >= launcher.config.item_count) {
        return;
    }

    char command[LAUNCHER_COMMAND_MAX];
    char *argv[SOLAR_OS_APP_ARG_MAX] = {0};
    strlcpy(command, launcher.config.items[launcher.selected].command,
            sizeof(command));
    const solar_os_shell_parse_result_t parsed =
        solar_os_shell_tokenize(command, argv, SOLAR_OS_APP_ARG_MAX);
    if (parsed.error != SOLAR_OS_SHELL_PARSE_OK || parsed.argc == 0) {
        launcher_finish_error(ctx, "invalid command", ESP_ERR_INVALID_ARG);
        return;
    }

    esp_err_t err = ESP_OK;
    const solar_os_app_registry_entry_t *entry =
        solar_os_app_registry_find(argv[0]);
    if (entry != NULL && entry->app != NULL) {
        err = launcher_launch_app(ctx, entry, parsed.argc, argv);
    } else if (parsed.argc == 1 && solar_os_shell_path_is_script(argv[0])) {
        err = launcher_launch_script(ctx, argv[0]);
    } else {
        err = solar_os_shell_execute_command(
            ctx, launcher.config.items[launcher.selected].command);
        if (err == ESP_OK) {
            err = launcher_make_requested_app_child(ctx);
        }
        if (err == ESP_OK && ctx->requested_app == NULL && !ctx->exit_requested) {
            solar_os_context_finish(ctx, 0, NULL);
        }
    }
    if (err != ESP_OK) {
        launcher_finish_error(ctx, "command failed", err);
    }
}

static esp_err_t launcher_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc > 2) {
        solar_os_context_finish(ctx, 2, "usage: launcher [config.json]");
        return ESP_OK;
    }
    esp_err_t err = argc == 2 ?
        solar_os_storage_resolve_path(solar_os_context_argv(ctx, 1),
                                      launcher.config_path,
                                      sizeof(launcher.config_path)) :
        solar_os_storage_default_path(LAUNCHER_CONFIG_FILE,
                                      launcher.config_path,
                                      sizeof(launcher.config_path));
    if (err == ESP_OK) {
        err = launcher_load_config();
    }
    if (err != ESP_OK) {
        launcher_finish_error(ctx, "config load failed", err);
        return ESP_OK;
    }

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!launcher_config_fits(gfx)) {
        launcher_finish_error(ctx, "grid does not fit display", ESP_ERR_INVALID_SIZE);
        return ESP_OK;
    }
    launcher.pointer_x = (int)solar_os_gfx_width(gfx) / 2;
    launcher.pointer_y = (int)solar_os_gfx_height(gfx) / 2;
    launcher.render_pending = true;
    solar_os_context_set_graphics_active(ctx, true);
    launcher_render(ctx);
    return ESP_OK;
}

static void launcher_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void launcher_suspend(solar_os_context_t *ctx)
{
    launcher.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void launcher_resume(solar_os_context_t *ctx)
{
    launcher.suspended = false;
    const esp_err_t err = launcher_load_config();
    if (err != ESP_OK) {
        launcher_finish_error(ctx, "config reload failed", err);
        return;
    }
    if (!launcher_config_fits(solar_os_context_gfx(ctx))) {
        launcher_finish_error(ctx, "grid does not fit display", ESP_ERR_INVALID_SIZE);
        return;
    }
    launcher.render_pending = true;
    solar_os_context_set_graphics_active(ctx, true);
    launcher_render(ctx);
}

static bool launcher_pointer_event(solar_os_context_t *ctx,
                                   const solar_os_input_pointer_event_t *pointer)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || pointer == NULL) {
        return true;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    if (pointer->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        launcher.pointer_relative = false;
        launcher.pointer_x = pointer->x;
        launcher.pointer_y = pointer->y;
    } else {
        launcher.pointer_relative = true;
        launcher.pointer_x += pointer->delta_x;
        launcher.pointer_y += pointer->delta_y;
    }
    if (launcher.pointer_x < 0) launcher.pointer_x = 0;
    if (launcher.pointer_y < 0) launcher.pointer_y = 0;
    if (launcher.pointer_x >= width) launcher.pointer_x = width - 1;
    if (launcher.pointer_y >= height) launcher.pointer_y = height - 1;

    solar_os_launcher_cell_t cells[LAUNCHER_ITEM_MAX];
    launcher_copy_cells(cells);
    const size_t hit = solar_os_launcher_layout_hit(
        cells, launcher.config.item_count,
        launcher.config.columns, launcher.config.rows,
        width, height, launcher.pointer_x, launcher.pointer_y);
    if (hit != SIZE_MAX && hit != launcher.selected) {
        launcher.selected = hit;
        launcher.render_pending = true;
    } else if (pointer->mode == SOLAR_OS_INPUT_POINTER_RELATIVE) {
        launcher.render_pending = true;
    }

    const bool primary_press = pointer->action == SOLAR_OS_INPUT_POINTER_PRESS &&
        (pointer->buttons == 0U ||
         (pointer->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY) != 0U);
    if (primary_press && hit != SIZE_MAX) {
        launcher.selected = hit;
        launcher_activate(ctx);
    }
    return true;
}

static bool launcher_char_event(solar_os_context_t *ctx, uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    int delta_column = 0;
    int delta_row = 0;
    if (key == SOLAR_OS_KEY_LEFT) delta_column = -1;
    if (key == SOLAR_OS_KEY_RIGHT) delta_column = 1;
    if (key == SOLAR_OS_KEY_UP) delta_row = -1;
    if (key == SOLAR_OS_KEY_DOWN) delta_row = 1;
    if (delta_column != 0 || delta_row != 0) {
        solar_os_launcher_cell_t cells[LAUNCHER_ITEM_MAX];
        launcher_copy_cells(cells);
        const size_t selected = solar_os_launcher_layout_move(
            cells, launcher.config.item_count, launcher.selected,
            delta_column, delta_row);
        if (selected != launcher.selected) {
            launcher.selected = selected;
            launcher.render_pending = true;
        }
        return true;
    }
    if (key == '\r' || key == '\n' || key == SOLAR_OS_KEY_ENTER) {
        launcher_activate(ctx);
    }
    return true;
}

static bool launcher_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_POINTER) {
        return launcher_pointer_event(ctx, &event->data.pointer);
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return launcher_char_event(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (launcher.render_pending) {
            launcher_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        launcher_resume(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_launcher_app = {
    .name = "launcher",
    .summary = "configurable graphical launcher",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = launcher_start,
    .suspend = launcher_suspend,
    .resume = launcher_resume,
    .stop = launcher_stop,
    .event = launcher_event,
    .state_slot = &launcher_state_storage,
    .state_size = sizeof(launcher_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = LAUNCHER_TICK_MS,
    .tick_deadline_ms = 25U,
};
