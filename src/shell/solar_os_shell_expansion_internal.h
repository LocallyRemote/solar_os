#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_expansion.h"

bool solar_os_shell_expansion_parse_binding_token(
    const char *arg,
    solar_os_expansion_binding_t *bindings,
    size_t *binding_count);
