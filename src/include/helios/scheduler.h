/**
 * @file scheduler.h
 * @brief Per-core O(1) priority-bitmap scheduler API.
 */

#ifndef HELIOS_SCHEDULER_H
#define HELIOS_SCHEDULER_H

#include <helios/types.h>
#include <helios/microprog.h>
#include <helios/sched/per_core.h>

/**
 * @brief Initialize the BSP scheduler — create idle thread, arm timer, sti.
 */
void scheduler_init(void);

/**
 * @brief Initialize scheduler for an AP core.
 * Creates the per-core idle thread. Called from ap_entry().
 */
void scheduler_init_core(void);

/**
 * @brief Enqueue a microprogram into a core's ready queue.
 */
void scheduler_enqueue(per_core_data_t *core, microprogram_t *mp);

/**
 * @brief Pick the highest-priority ready task (O(1) via bitmap).
 * @return The next task to run, or the idle thread if all queues empty.
 */
microprogram_t *scheduler_pick_next(per_core_data_t *core);

/**
 * @brief Timer interrupt handler — check sleeping, preempt if needed.
 * Called from interrupt_dispatch for vector TIMER_VECTOR.
 */
void scheduler_preempt(void);

/**
 * @brief Voluntarily yield the current timeslice.
 */
void scheduler_yield(void);

/**
 * @brief Block the current task with the given state.
 * Removes from run queue, picks next, and context-switches.
 */
void scheduler_block(mprog_state_t reason);

/**
 * @brief Wake a blocked task — set READY and enqueue.
 */
void scheduler_wake(microprogram_t *mp);

/**
 * @brief Check sleeping list and wake tasks whose deadline has passed.
 */
void scheduler_check_sleeping(per_core_data_t *core);

/**
 * @brief Try to steal work from another core's ready queue.
 * @return true if a task was stolen.
 */
bool scheduler_try_steal(per_core_data_t *core);

/**
 * @brief Arm the APIC timer for the current task's timeslice.
 */
void scheduler_arm_timer(per_core_data_t *core);

/**
 * @brief Idle loop — entered by each core after init. Never returns.
 * Checks queues → try_steal → hlt. Ensures IF=1 before HLT.
 */
NORETURN void scheduler_idle_loop(void);

/** Global flag: true once scheduler_init() has completed. */
extern bool g_scheduler_online;

#endif /* HELIOS_SCHEDULER_H */
