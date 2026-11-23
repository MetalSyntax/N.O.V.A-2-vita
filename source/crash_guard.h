/*
 * Crash guard: turns fatal errors (data/prefetch aborts, abort(), uncaught
 * C++ exceptions, stack smashing, Android asserts) into a report file plus an
 * on-screen message, then exits cleanly instead of freezing / dumping.
 *
 * The process cannot keep running after these (memory is already corrupt or
 * the faulting code cannot continue); the goal is to tell the user and the
 * developer WHY it failed.
 */
#ifndef CRASH_GUARD_H
#define CRASH_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once, as early as possible (after the .so is loaded).
void crash_guard_init(void);

// Report a fatal error from the current thread. Never returns.
// regs: r0-r12, sp, lr, pc (16 words) or NULL.
void crash_fatal(const char *kind, const char *detail, uint32_t pc, uint32_t lr,
                 const uint32_t *regs, uint32_t far) __attribute__((noreturn));

// Called by the render thread every frame: if another thread crashed, park
// here so the crash screen can take over the GPU.
void crash_guard_poll(void);

#ifdef __cplusplus
}
#endif

#endif
