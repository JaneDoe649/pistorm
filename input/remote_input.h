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
 *   P                          ping, answered with "PONG"
 */

int remote_input_start(unsigned int port);
void remote_input_stop(void);

#endif
