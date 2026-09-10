#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t column;
    uint8_t row;
} solar_os_launcher_cell_t;

bool solar_os_launcher_layout_valid(const solar_os_launcher_cell_t *cells,
                                    size_t count,
                                    uint8_t columns,
                                    uint8_t rows);
size_t solar_os_launcher_layout_move(const solar_os_launcher_cell_t *cells,
                                     size_t count,
                                     size_t current,
                                     int delta_column,
                                     int delta_row);
size_t solar_os_launcher_layout_hit(const solar_os_launcher_cell_t *cells,
                                    size_t count,
                                    uint8_t columns,
                                    uint8_t rows,
                                    int width,
                                    int height,
                                    int x,
                                    int y);
