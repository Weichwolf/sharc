// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef __cplusplus
#include <cstdint>
using sharc_control_word = std::uint64_t;
extern "C" {
#else
#include <stdint.h>
typedef uint64_t sharc_control_word;
#endif

int sharc_native_invoke(void (*entry)(void **, sharc_control_word *),
                        void **pointers,
                        sharc_control_word *control,
                        void **landing);
[[noreturn]] void sharc_native_escape(void *landing);

#ifdef __cplusplus
}
#endif
