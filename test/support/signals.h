// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef __cplusplus
using sharc_test_claim = int (*)(const void *, int, unsigned long);
extern "C" {
#else
typedef int (*sharc_test_claim)(const void *, int, unsigned long);
#endif

int sharc_test_signals_begin(sharc_test_claim claim);
void sharc_test_signals_end(void);
int sharc_test_mask_restored(void);
unsigned sharc_test_signal_count(void);
int sharc_test_last_was_bus(void);
int sharc_test_chain_previous(void);
int sharc_test_wait_for_child(int child);

#ifdef __cplusplus
}
#endif
