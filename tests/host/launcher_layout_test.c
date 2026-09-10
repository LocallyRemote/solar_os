#include <assert.h>
#include <stdio.h>

#include "solar_os_launcher_layout.h"

int main(void)
{
    const solar_os_launcher_cell_t cells[] = {
        {.column = 0U, .row = 0U},
        {.column = 2U, .row = 0U},
        {.column = 1U, .row = 1U},
        {.column = 2U, .row = 1U},
    };
    assert(solar_os_launcher_layout_valid(cells, 4U, 3U, 2U));
    assert(solar_os_launcher_layout_move(cells, 4U, 0U, 1, 0) == 1U);
    assert(solar_os_launcher_layout_move(cells, 4U, 1U, 0, 1) == 3U);
    assert(solar_os_launcher_layout_move(cells, 4U, 3U, -1, 0) == 2U);
    assert(solar_os_launcher_layout_move(cells, 4U, 0U, -1, 0) == 0U);
    assert(solar_os_launcher_layout_hit(cells, 4U, 3U, 2U,
                                        300, 200, 250, 50) == 1U);
    assert(solar_os_launcher_layout_hit(cells, 4U, 3U, 2U,
                                        300, 200, 10, 150) == SIZE_MAX);

    const solar_os_launcher_cell_t duplicate[] = {
        {.column = 1U, .row = 1U},
        {.column = 1U, .row = 1U},
    };
    assert(!solar_os_launcher_layout_valid(duplicate, 2U, 3U, 2U));
    puts("launcher layout tests: ok");
    return 0;
}
