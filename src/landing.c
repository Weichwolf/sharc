// SPDX-License-Identifier: GPL-3.0-only
#include "landing.h"

#include <setjmp.h>

int sharc_native_invoke(void (*entry)(void **, sharc_control_word *),
                        void **pointers,
                        sharc_control_word *control,
                        void **landing) {
  sigjmp_buf target;
  // The consumer restores the interrupted mask on the cold fault path.
  // Saving no mask keeps a successful native entry free of mask syscalls.
  if (sigsetjmp(target, 0) != 0) { return 0; }
  *landing = &target;
  entry(pointers, control);
  return 1;
}

[[noreturn]] void sharc_native_escape(void *landing) {
  siglongjmp(*(sigjmp_buf *)landing, 1);
}
