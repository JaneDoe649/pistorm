// SPDX-License-Identifier: MIT

#ifndef REMOTE_DIAG_H
#define REMOTE_DIAG_H

/**
 * Diagnostics for the remote input (freezes, keyboard, joystick), built only with REMOTE_DIAG:
 *
 *   make REMOTE_DIAG=1 PLATFORM=...
 *
 * They count and log accesses to the CIA / chipset registers, sample the CPU and dump memory when the key queue
 * is stuck. They are verbose and cost a few atomic operations on the hooked accesses, hence off by default.
 * Without REMOTE_DIAG everything below compiles to nothing.
 */

#include <stdint.h>

#ifdef REMOTE_DIAG

#define DIAG(...) __VA_ARGS__
#define DIAG_COUNT(v) __atomic_fetch_add(&(v), 1, __ATOMIC_RELAXED)

// counters bumped by the CPU thread on the hooked accesses, read by the remote input thread
extern uint32_t remote_diag_icr, remote_diag_dat, remote_diag_intreq; // ICR reads answered with SP, SDR pops, PORTS acks
extern uint32_t remote_diag_joydat[2][3];                             // JOY0DAT/JOY1DAT reads by width
extern uint32_t remote_diag_pra;                                      // CIAAPRA reads while the remote joystick is active
extern int32_t remote_diag_pra_type, remote_diag_pra_fire, remote_diag_ddra, remote_diag_potgo; // -1: never seen

// CPU thread: CIAADAT access (kind: 0 real register, 1 same key read again, 2 new key latched)
void remote_diag_log_dat(uint8_t key, uint8_t event, int kind);
// CPU thread: a CIA / chipset register was read while the joystick fire is down
void remote_diag_fire_census_hit(uint8_t type, uint32_t addr);
// CPU thread: INTENA write as seen by the hook
void remote_diag_log_intena(uint8_t type, uint32_t addr, uint32_t val);
// CPU thread, every loop: copies the memory requested by the remote input thread (only this thread may use the bus)
void remote_diag_cpu_poll(void);
// remote input thread, every loop: stuck queue report, joystick and CIAADAT logs
void remote_diag_poll(void);
// remote input thread: a joystick command was received
void remote_diag_joystick(int mask);

#else

#define DIAG(...)
#define DIAG_COUNT(v) ((void)0)
#define remote_diag_cpu_poll() ((void)0)
#define remote_diag_poll() ((void)0)

#endif

#endif
