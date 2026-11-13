// SPDX-License-Identifier: MIT

#include <stdint.h>

enum keypress_type {
  KEYPRESS_RELEASE,
  KEYPRESS_PRESS,
  KEYPRESS_REPEAT,
};

int get_mouse_status(uint8_t *x, uint8_t *y, uint8_t *b, uint8_t *e);
int get_key_char(char *c, char *code, char *event_type);
int queue_keypress(uint8_t keycode, uint8_t event_type, uint8_t platform);
int queue_remote_keypress(uint8_t keycode, uint8_t event_type, uint8_t platform);
void inject_mouse_move(int dx, int dy);
void inject_mouse_buttons(uint8_t buttons);
void clear_injected_mouse(void);
void remote_mouse_poll(void);
uint8_t remote_mouse_buttons(void);
void inject_joystick(uint8_t mask);
void clear_injected_joystick(void);
int remote_joystick_present(void);
uint8_t remote_joystick_mask(void);
uint16_t remote_joystick_dat(void);
extern uint8_t remote_mouse_x, remote_mouse_y;
int get_num_kb_queued();
void pop_queued_key(uint8_t *c, uint8_t *t);
// emulated CIA-A serial data register (keyboard), see input.c
int kb_sdr_read(int may_latch, uint8_t *c, uint8_t *t);
int kb_icr_sp_take(int may_latch);
void kb_sdr_ack(void);
void kb_sdr_tick(void);
int kb_irq_wanted(void);
void kb_request_irq(void);
int kb_irq_poll(void);
extern uint32_t kb_sdr_timeouts;
int grab_device(int fd);
int release_device(int fd);
