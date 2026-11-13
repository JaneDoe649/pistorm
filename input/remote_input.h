// SPDX-License-Identifier: MIT

#ifndef REMOTE_INPUT_H
#define REMOTE_INPUT_H

/**
 * Remote keyboard/mouse injection over TCP.
 *
 * Listens on 127.0.0.1:<port> only; reach it from another machine through an SSH tunnel
 * (ssh -L <port>:127.0.0.1:<port> user@pi). One client at a time; a new connection replaces the old one.
 *
 * Line based protocol (ASCII, one command per line):
 *   K <linux_key_code> <1|0>   key press / release (Linux KEY_* codes; caps lock: send 1 on each toggle)
 *   M <dx> <dy>                relative mouse movement (+dx right, +dy down)
 *   B <mask>                   mouse buttons state (bit 0 left, bit 1 right, bit 2 middle)
 *   W <n>                      mouse wheel ticks (+ up, - down), at most 5 per line
 *   J <mask>                   joystick on Amiga port 2: 1 up, 2 down, 4 left, 8 right, 16 fire, 32 2nd button
 *                              (0 = centered, the real port 2 is read again)
 *   P                          ping, answered with "PONG"
 */

#include <stdint.h>

int remote_input_start(unsigned int port);
void remote_input_stop(void);

// diagnostics: called by the CPU thread on every CIAADAT read while remote input is active
// (kind: 0 real register, 1 same key read again, 2 new key latched)
void remote_diag_log_dat(uint8_t key, uint8_t event, int kind);

#endif
