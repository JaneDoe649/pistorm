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
void inject_mouse_wheel(int steps);
void clear_injected_mouse(void);
int get_num_kb_queued();
void pop_queued_key(uint8_t *c, uint8_t *t);
int grab_device(int fd);
int release_device(int fd);
