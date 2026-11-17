// SPDX-License-Identifier: GPL-3.0-only
#include "signals.h"

#include <signal.h>
#include <stdatomic.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <bits/types/siginfo_t.h>
#include <bits/types/sigset_t.h>
#endif

enum { handler_failure = 90, unclaimed_fault = 91, unexpected_return = 92 };
static struct sigaction previous_segv, previous_bus;
static sigset_t original_mask, working_mask;
static sharc_test_claim claim_fault;
static atomic_uint signal_count;
static atomic_int last_signal;

static void handle_fault(int number, siginfo_t *info, void *context) {
  const ucontext_t *interrupted = context;
  sigset_t handler_mask;
  if (pthread_sigmask(SIG_SETMASK, nullptr, &handler_mask) != 0 ||
      pthread_sigmask(SIG_SETMASK, &interrupted->uc_sigmask, nullptr) != 0) {
    _exit(handler_failure);
  }
  atomic_fetch_add_explicit(&signal_count, 1, memory_order_relaxed);
  atomic_store_explicit(&last_signal, number, memory_order_relaxed);
  const int writing = (interrupted->uc_mcontext.gregs[REG_ERR] & 2) != 0;
  const unsigned long pc = (unsigned long)interrupted->uc_mcontext.gregs[REG_RIP];
  if (!claim_fault(info->si_addr, writing, pc)) {
    if (pthread_sigmask(SIG_SETMASK, &handler_mask, nullptr) != 0) { _exit(handler_failure); }
    const struct sigaction *previous = number == SIGSEGV ? &previous_segv : &previous_bus;
    if ((previous->sa_flags & SA_SIGINFO) != 0) {
      previous->sa_sigaction(number, info, context);
    } else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) {
      previous->sa_handler(number);
    } else {
      // A default disposition is a failing fixture, never a recovered fault.
      _exit(unclaimed_fault);
    }
  }
  _exit(unexpected_return);
}

int sharc_test_signals_begin(sharc_test_claim claim) {
  if (!atomic_is_lock_free(&signal_count) || !atomic_is_lock_free(&last_signal)) { return -1; }
  if (pthread_sigmask(SIG_SETMASK, nullptr, &original_mask) != 0) { return -1; }
  working_mask = original_mask;
  if (sigaddset(&working_mask, SIGUSR1) != 0 || sigdelset(&working_mask, SIGUSR2) != 0 ||
      sigdelset(&working_mask, SIGSEGV) != 0 || sigdelset(&working_mask, SIGBUS) != 0 ||
      pthread_sigmask(SIG_SETMASK, &working_mask, nullptr) != 0) {
    return -1;
  }
  claim_fault = claim;
  struct sigaction action = {};
  action.sa_sigaction = handle_fault;
  action.sa_flags = SA_SIGINFO;
  if (sigemptyset(&action.sa_mask) != 0 || sigaddset(&action.sa_mask, SIGUSR2) != 0 ||
      sigaction(SIGSEGV, &action, &previous_segv) != 0 ||
      sigaction(SIGBUS, &action, &previous_bus) != 0) {
    return -1;
  }
  return 0;
}

void sharc_test_signals_end(void) {
  (void)sigaction(SIGSEGV, &previous_segv, nullptr);
  (void)sigaction(SIGBUS, &previous_bus, nullptr);
  (void)pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
}

int sharc_test_mask_restored(void) {
  sigset_t current;
  if (pthread_sigmask(SIG_SETMASK, nullptr, &current) != 0) { return 0; }
  for (int number = 1; number < NSIG; ++number) {
    if (sigismember(&current, number) != sigismember(&working_mask, number)) { return 0; }
  }
  return 1;
}

unsigned sharc_test_signal_count(void) {
  return atomic_load_explicit(&signal_count, memory_order_relaxed);
}

int sharc_test_last_was_bus(void) {
  return atomic_load_explicit(&last_signal, memory_order_relaxed) == SIGBUS;
}

static void chained(int number, siginfo_t *info, void *context) {
  (void)info;
  (void)context;
  sigset_t current;
  const int restored = pthread_sigmask(SIG_SETMASK, nullptr, &current) == 0 &&
                       sigismember(&current, SIGUSR1) == 1 && sigismember(&current, SIGUSR2) == 1 &&
                       sigismember(&current, number) == 1;
  _exit(restored ? 0 : handler_failure);
}

int sharc_test_chain_previous(void) {
  struct sigaction previous = {};
  previous.sa_sigaction = chained;
  previous.sa_flags = SA_SIGINFO;
  return sigemptyset(&previous.sa_mask) == 0 ? sigaction(SIGSEGV, &previous, nullptr) : -1;
}

int sharc_test_wait_for_child(int child) {
  int status = 0;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0
                                                                                              : -1;
}
