#include "solar_os_launcher_layout.h"

#include <limits.h>

bool solar_os_launcher_layout_valid(const solar_os_launcher_cell_t *cells,
                                    size_t count,
                                    uint8_t columns,
                                    uint8_t rows)
{
    if (cells == NULL || count == 0U || columns == 0U || rows == 0U) {
        return false;
    }
    for (size_t i = 0U; i < count; i++) {
        if (cells[i].column >= columns || cells[i].row >= rows) {
            return false;
        }
        for (size_t j = 0U; j < i; j++) {
            if (cells[i].column == cells[j].column &&
                cells[i].row == cells[j].row) {
                return false;
            }
        }
    }
    return true;
}

size_t solar_os_launcher_layout_move(const solar_os_launcher_cell_t *cells,
                                     size_t count,
                                     size_t current,
                                     int delta_column,
                                     int delta_row)
{
    if (cells == NULL || current >= count ||
        ((delta_column == 0) == (delta_row == 0))) {
        return current;
    }

    size_t best = current;
    unsigned best_score = UINT_MAX;
    for (size_t i = 0U; i < count; i++) {
        if (i == current) {
            continue;
        }
        const int column_distance =
            (int)cells[i].column - (int)cells[current].column;
        const int row_distance = (int)cells[i].row - (int)cells[current].row;
        const int primary = delta_column != 0 ?
            column_distance * delta_column : row_distance * delta_row;
        if (primary <= 0) {
            continue;
        }
        const int cross = delta_column != 0 ? row_distance : column_distance;
        const unsigned orthogonal = (unsigned)(cross < 0 ? -cross : cross);
        const unsigned score = orthogonal * 256U + (unsigned)primary;
        if (score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

size_t solar_os_launcher_layout_hit(const solar_os_launcher_cell_t *cells,
                                    size_t count,
                                    uint8_t columns,
                                    uint8_t rows,
                                    int width,
                                    int height,
                                    int x,
                                    int y)
{
    if (cells == NULL || columns == 0U || rows == 0U ||
        width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x >= width || y >= height) {
        return SIZE_MAX;
    }
    const uint8_t column = (uint8_t)((int64_t)x * columns / width);
    const uint8_t row = (uint8_t)((int64_t)y * rows / height);
    for (size_t i = 0U; i < count; i++) {
        if (cells[i].column == column && cells[i].row == row) {
            return i;
        }
    }
    return SIZE_MAX;
}
