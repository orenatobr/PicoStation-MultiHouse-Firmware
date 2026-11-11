// Portability shim: the Multi House code builds for the RP2040 and, for unit tests, on the host.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(PICO_ON_DEVICE) || defined(PICO_BUILD)
#include "pico.h"
#define MH_RAM_FUNC(name) __time_critical_func(name)
#else
// Host build (tests/): no placement attributes, no hardware.
#define MH_RAM_FUNC(name) name
#define __not_in_flash_func(name) name
#define __time_critical_func(name) name
#endif
