// SPDX-License-Identifier: MIT

#include <linux/input.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "platforms/platforms.h"
#include "input.h"

#define NONE 0x80

static int lshift = 0, rshift = 0,/* lctrl = 0, rctrl = 0,*/ lalt = 0, altgr = 0, capslk = 0;
extern int mouse_fd;
extern int keyboard_fd;

// n.b. $fe and $ff are mapped to newmouse standard wheel up/down keycodes, nonexistant on amiga keyboards
char keymap_amiga[256] = {
/*      00    01    02    03    04    05    06    07    08    09    0A    0B    0C    0D    0E    0F */
/*00*/ 0x80, 0x45, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x41, 0x42,
/*10*/ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x44, 0x63, 0x20, 0x21,
/*20*/ 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x00, 0x60, 0x2B, 0x31, 0x32, 0x33, 0x34,
/*30*/ 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x61, 0x5D, 0x64, 0x40, 0x62, 0x50, 0x51, 0x52, 0x53, 0x54,
/*40*/ 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, NONE, 0x3D, 0x3E, 0x3F, 0x4A, 0x2D, 0x2E, 0x2F, 0x5E, 0x1D,
/*50*/ 0x1E, 0x1F, 0x0F, 0x3C, NONE, NONE, 0x30, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*60*/ 0x43, NONE, 0x5C, NONE, 0x65, NONE, 0x5F, 0x4C, 0x5A, 0x4F, 0x4E, NONE, 0x4D, NONE, 0x0D, 0x46,
/*70*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, 0x66, 0x67, 0x67,
/*80*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*90*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*A0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*B0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*C0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*D0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*E0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
/*F0*/ NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, 0x7A, 0x7B,
};

int handle_modifier(struct input_event *ev) {
  int *target_modifier = NULL;
  if (ev->value != KEYPRESS_REPEAT && (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT || ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT || ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL)) {
    switch(ev->code) {
      case KEY_LEFTSHIFT:
        target_modifier = &lshift;
        break;
      case KEY_RIGHTSHIFT:
        target_modifier = &rshift;
        break;
      case KEY_LEFTALT:
        target_modifier = &lalt;
        break;
      case KEY_RIGHTALT:
        target_modifier = &altgr;
        break;
      case KEY_LEFTCTRL:
        target_modifier = &lshift;
        break;
      case KEY_RIGHTCTRL:
        target_modifier = &rshift;
        break;
    }
    *target_modifier = (ev->value == KEYPRESS_RELEASE) ? 0 : 1;
    return 1;
  } else if (ev->value == KEYPRESS_PRESS && ev->code == KEY_CAPSLOCK) {
    capslk = !capslk;
    return 1;
  } else {
    return 0;
  }
}

#define KEYCASE(a, b, c)case a: return (((lshift || rshift) && !capslk) || (!(lshift || rshift) && capslk)) ? (c) : (b);

/**
 * translates keycodes back into a simpler enumerable value for handling emulator command events
 *
 * @param *struct/input_event  ev  pointer to input layer event structure
 * @return char
 */
char char_from_input_event(struct input_event *ev) {
  switch(ev->code) {
    KEYCASE(KEY_A, 'a', 'A');
    KEYCASE(KEY_B, 'b', 'B');
    KEYCASE(KEY_C, 'c', 'C');
    KEYCASE(KEY_D, 'd', 'D');
    KEYCASE(KEY_E, 'e', 'E');
    KEYCASE(KEY_F, 'f', 'F');
    KEYCASE(KEY_G, 'g', 'G');
    KEYCASE(KEY_H, 'h', 'H');
    KEYCASE(KEY_I, 'i', 'I');
    KEYCASE(KEY_J, 'j', 'J');
    KEYCASE(KEY_K, 'k', 'K');
    KEYCASE(KEY_L, 'l', 'L');
    KEYCASE(KEY_M, 'm', 'M');
    KEYCASE(KEY_N, 'n', 'N');
    KEYCASE(KEY_O, 'o', 'O');
    KEYCASE(KEY_P, 'p', 'P');
    KEYCASE(KEY_Q, 'q', 'Q');
    KEYCASE(KEY_R, 'r', 'R');
    KEYCASE(KEY_S, 's', 'S');
    KEYCASE(KEY_T, 't', 'T');
    KEYCASE(KEY_U, 'u', 'U');
    KEYCASE(KEY_V, 'v', 'V');
    KEYCASE(KEY_W, 'w', 'W');
    KEYCASE(KEY_X, 'x', 'X');
    KEYCASE(KEY_Y, 'y', 'Y');
    KEYCASE(KEY_Z, 'z', 'Z');
    KEYCASE(KEY_1, '1', '!');
    KEYCASE(KEY_2, '2', '@');
    KEYCASE(KEY_3, '3', '#');
    KEYCASE(KEY_4, '4', '$');
    KEYCASE(KEY_5, '5', '%');
    KEYCASE(KEY_6, '6', '^');
    KEYCASE(KEY_7, '7', '&');
    KEYCASE(KEY_8, '8', '*');
    KEYCASE(KEY_9, '9', '(');
    KEYCASE(KEY_0, '0', ')');
    KEYCASE(KEY_F12, 0x1B, 0x1B);
    KEYCASE(KEY_PAUSE, 0x01, 0x01);
    default:
      return 0;
  }
}

int get_key_char(char *c, char *code, char *event_type)
{
  if (keyboard_fd == -1)
    return 0;

  struct input_event ie;
  while(read(keyboard_fd, &ie, sizeof(struct input_event)) != -1) {
    if (ie.type == EV_KEY) {
      handle_modifier(&ie);
      char ret = char_from_input_event(&ie);
      *c = ret;
      *code = ie.code;
      *event_type = ie.value;
      return 1;
    }
  }

  return 0;
}

uint8_t mouse_x = 0, mouse_y = 0;

int get_mouse_status(uint8_t *x, uint8_t *y, uint8_t *b, uint8_t *e) {
  uint8_t mouse_ev[4];
  if (read(mouse_fd, &mouse_ev, 4) != -1) {
    *b = mouse_ev[0];
    *e = mouse_ev[3];

    mouse_x += mouse_ev[1];
    *x = mouse_x;
    mouse_y += (-mouse_ev[2]);
    *y = mouse_y;
    return 1;
  }

  return 0;
}

/**
 * Remote (injected) mouse. The remote thread accumulates deltas and button state with atomics, the CPU thread
 * calls remote_mouse_poll() every loop iteration to turn them into the counters below.
 *
 * remote_mouse_x/y are NOT a replacement for the Amiga mouse: they are an offset that platform_read_check()
 * adds to the real JOY0DAT counters, so the physical mouse of the Amiga keeps working. Once non-zero the
 * offset has to stay applied forever, as the Amiga only looks at differences between two reads.
 * Deltas use screen orientation: +dx = right, +dy = down.
 */
#define REMOTE_PENDING_LIMIT 4096
#define REMOTE_STEP_MAX 16
static int32_t remote_dx = 0, remote_dy = 0;
static uint8_t remote_buttons = 0;
uint8_t remote_mouse_x = 0, remote_mouse_y = 0; // CPU thread only

static void remote_accumulate(int32_t *target, int delta) {
  int32_t cur = __atomic_load_n(target, __ATOMIC_RELAXED);
  // don't let the backlog grow without bound if the CPU thread isn't consuming
  if (cur + delta > REMOTE_PENDING_LIMIT || cur + delta < -REMOTE_PENDING_LIMIT)
    return;
  __atomic_fetch_add(target, delta, __ATOMIC_RELAXED);
}

void inject_mouse_move(int dx, int dy) {
  if (dx)
    remote_accumulate(&remote_dx, dx);
  if (dy)
    remote_accumulate(&remote_dy, dy);
}

void inject_mouse_buttons(uint8_t buttons) {
  // bit 0: left, bit 1: right, bit 2: middle
  __atomic_store_n(&remote_buttons, buttons & 0x07, __ATOMIC_RELAXED);
}

void clear_injected_mouse(void) {
  // pending movement and buttons are dropped; the counters already applied must stay (see above)
  __atomic_store_n(&remote_dx, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&remote_dy, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&remote_buttons, 0, __ATOMIC_RELAXED);
}

uint8_t remote_mouse_buttons(void) {
  return __atomic_load_n(&remote_buttons, __ATOMIC_RELAXED);
}

/**
 * Remote joystick, presented on Amiga port 2 (JOY1DAT, fire on CIAAPRA bit 7, second button on POTGOR bit 14).
 * Mask: 1 up, 2 down, 4 left, 8 right, 16 fire, 32 button 2. While the remote joystick has a direction or
 * button pressed ("present"), it replaces whatever is on port 2; otherwise reads go to the real port.
 */
static uint8_t remote_joy = 0, remote_joy_on = 0;

void inject_joystick(uint8_t mask) {
  mask &= 0x3F;
  __atomic_store_n(&remote_joy, mask, __ATOMIC_RELAXED);
  // a centered remote joystick hands the port back to the real one
  __atomic_store_n(&remote_joy_on, mask != 0, __ATOMIC_RELAXED);
}

void clear_injected_joystick(void) {
  __atomic_store_n(&remote_joy, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&remote_joy_on, 0, __ATOMIC_RELAXED);
}

int remote_joystick_present(void) {
  return __atomic_load_n(&remote_joy_on, __ATOMIC_RELAXED);
}

uint8_t remote_joystick_mask(void) {
  return __atomic_load_n(&remote_joy, __ATOMIC_RELAXED);
}

// JOY1DAT word for a digital joystick: bit 1 right, bit 9 left, bit 0 = down^right, bit 8 = up^left.
// Opposite directions cancel out.
uint16_t remote_joystick_dat(void) {
  uint8_t m = remote_joystick_mask();
  int up = m & 1, down = (m >> 1) & 1, left = (m >> 2) & 1, right = (m >> 3) & 1;
  if (up && down)
    up = down = 0;
  if (left && right)
    left = right = 0;
  uint16_t v = 0;
  if (right)
    v |= 0x0002;
  if (left)
    v |= 0x0200;
  if (down ^ right)
    v |= 0x0001;
  if (up ^ left)
    v |= 0x0100;
  return v;
}

static inline int32_t clamp_step(int32_t v) {
  return v > REMOTE_STEP_MAX ? REMOTE_STEP_MAX : (v < -REMOTE_STEP_MAX ? -REMOTE_STEP_MAX : v);
}

// Called from the CPU thread only. Moves are consumed in small steps so that the 8 bit counters never jump
// by more than the Amiga can disambiguate between two reads of JOY0DAT.
void remote_mouse_poll(void) {
  int32_t pdx = __atomic_load_n(&remote_dx, __ATOMIC_RELAXED);
  int32_t pdy = __atomic_load_n(&remote_dy, __ATOMIC_RELAXED);
  if (pdx || pdy) {
    int32_t sx = clamp_step(pdx), sy = clamp_step(pdy);
    __atomic_fetch_sub(&remote_dx, sx, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&remote_dy, sy, __ATOMIC_RELAXED);
    remote_mouse_x += (uint8_t)sx;
    remote_mouse_y += (uint8_t)sy;
  }
}

/**
 * The key queue is written by the local keyboard thread and the remote input thread, and read by the
 * CPU thread, so every access to the ring buffer goes through kb_queue_lock. The CPU thread only takes
 * it when the Amiga actually reads a key (CIAADAT), never in its main loop.
 */
static pthread_mutex_t kb_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t queued_keypresses = 0, queue_output_pos = 0, queue_input_pos = 0;
static uint8_t queued_keys[256];
static uint8_t queued_events[256];

void clear_keypress_queue() {
  pthread_mutex_lock(&kb_queue_lock);
  memset(queued_keys, 0x80, 256);
  memset(queued_events, 0x80, 256);
  __atomic_store_n(&queued_keypresses, 0, __ATOMIC_RELAXED);
  queue_output_pos = 0;
  queue_input_pos = 0;
  pthread_mutex_unlock(&kb_queue_lock);
}

static int queue_keypress_locked(uint8_t keycode, uint8_t event_type, uint8_t platform);

int queue_keypress(uint8_t keycode, uint8_t event_type, uint8_t platform) {
  pthread_mutex_lock(&kb_queue_lock);
  int ret = queue_keypress_locked(keycode, event_type, platform);
  pthread_mutex_unlock(&kb_queue_lock);
  return ret;
}

// Same as queue_keypress, but also tracks the caps lock state, which for local keys is done by handle_modifier
int queue_remote_keypress(uint8_t keycode, uint8_t event_type, uint8_t platform) {
  pthread_mutex_lock(&kb_queue_lock);
  if (keycode == KEY_CAPSLOCK && event_type == KEYPRESS_PRESS)
    capslk = !capslk;
  int ret = queue_keypress_locked(keycode, event_type, platform);
  pthread_mutex_unlock(&kb_queue_lock);
  return ret;
}

static int queue_keypress_locked(uint8_t keycode, uint8_t event_type, uint8_t platform) {
  char *keymap = NULL;
  switch (platform) {
    case PLATFORM_AMIGA:
      if (event_type != KEYPRESS_REPEAT)
        keymap = keymap_amiga;
      break;
    default:
      break;
  }
  if (keymap != NULL) {
    if ((uint8_t)keymap[keycode] != NONE) {
      if (__atomic_load_n(&queued_keypresses, __ATOMIC_RELAXED) < 255) {
        // printf("Keypress queued, matched %.2X to host key code %.2X\n", keycode, keymap[keycode]);
        if (keycode == KEY_CAPSLOCK) {
          if (event_type == KEYPRESS_RELEASE) {
            return 0;
          }
          event_type = capslk;
        }
        queued_keys[queue_output_pos] = keymap[keycode];
        queued_events[queue_output_pos] = event_type;
        queue_output_pos++;
        __atomic_fetch_add(&queued_keypresses, 1, __ATOMIC_RELAXED);
        return 1;
      }
    }
  }
  return 0;
}

int get_num_kb_queued() {
  return __atomic_load_n(&queued_keypresses, __ATOMIC_RELAXED);
}

void pop_queued_key(uint8_t *c, uint8_t *t) {
  pthread_mutex_lock(&kb_queue_lock);
  if (queued_keypresses == 0) {
    *c = NONE;
    *t = NONE;
  } else {
    *c = queued_keys[queue_input_pos];
    *t = queued_events[queue_input_pos];
    queue_input_pos++;
    __atomic_fetch_sub(&queued_keypresses, 1, __ATOMIC_RELAXED);
  }
  pthread_mutex_unlock(&kb_queue_lock);
}

int grab_device(int fd) {
  int rc = 0;
  rc = ioctl(fd, EVIOCGRAB, (void *)1);
  return rc;
}

int release_device(int fd) {
  int rc = 0;
  rc = ioctl(fd, EVIOCGRAB, (void *)0);
  return rc;
}
