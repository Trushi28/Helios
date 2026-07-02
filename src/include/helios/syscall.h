/**
 * @file syscall.h
 * @brief SYSCALL/SYSRET setup and system call dispatch.
 */

#ifndef HELIOS_SYSCALL_H
#define HELIOS_SYSCALL_H

#include <helios/types.h>

/* System call numbers */
#define SYS_YIELD   0
#define SYS_EXIT    1
#define SYS_SLEEP   2
#define SYS_SPAWN   3

/**
 * @brief Initialize SYSCALL/SYSRET MSRs on the current core.
 *
 * Sets LSTAR, STAR, SFMASK, and enables EFER.SCE.
 * Must be called on every core (BSP and APs) since these are per-core MSRs.
 */
void syscall_init(void);

/**
 * @brief C dispatcher for system calls.
 *
 * Called from syscall_entry.asm after register save.
 * @param syscall_nr  System call number (from RAX).
 * @param arg1        First argument (from RDI).
 * @param arg2        Second argument (from RSI).
 * @param arg3        Third argument (from RDX).
 * @return Result value placed in RAX on return.
 */
uint64_t syscall_dispatch(uint64_t syscall_nr, uint64_t arg1,
                           uint64_t arg2, uint64_t arg3);

#endif /* HELIOS_SYSCALL_H */
