// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // pthread_setname_np
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "input/input.h"
#include "m68k.h"
#include "input/remote_input.h"
#include "platforms/amiga/amiga-interrupts.h"
#include "platforms/platforms.h"

// defined in emulator.c
extern int remote_kb_active;
extern uint32_t last_irq;
extern int irq;                // set by ipl_task: an interrupt is pending, the CPU then runs in tiny slices
extern uint16_t emulated_irqs; // platforms/amiga/amiga-interrupts.c
extern uint32_t last_last_irq;
extern uint8_t ipl_enabled[8];
extern uint32_t remote_diag_icr, remote_diag_dat, remote_diag_intreq; // emulator.c

#define LINE_MAX_LEN 128
// how often the thread wakes up to retry queued releases / re-raise the keyboard interrupt
#define RETRY_INTERVAL_MSEC 5
#define IDLE_INTERVAL_MSEC 200

static pthread_t remote_tid;
static int remote_running = 0;
static volatile int remote_stop = 0;
static unsigned int remote_port = 0;

static int listen_fd = -1, client_fd = -1;
static uint8_t key_down[256], key_release_pending[256], key_held_reported[256];
static int64_t key_down_at[256];
static int pending_releases = 0;

#define STUCK_QUEUE_MSEC 800
#define KEY_HELD_MSEC 800 // log a key that stays down longer than this: its release has not reached us

static int64_t now_msec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Reads of CIAADAT by the Amiga, recorded by the CPU thread (no printf there) and printed by this thread.
// Each read pops one queued key, whereas the real register can be read any number of times: two readers
// (or a handler reading twice) would swallow events, which is what this log is meant to show.
#define DAT_LOG_SIZE 64
struct dat_log_entry { uint8_t key, event, popped; uint32_t icr, dat, acks; };
static struct dat_log_entry dat_log[DAT_LOG_SIZE];
static uint32_t dat_log_head = 0, dat_log_printed = 0;

void remote_diag_log_dat(uint8_t key, uint8_t event, int popped) {
  uint32_t h = __atomic_load_n(&dat_log_head, __ATOMIC_RELAXED);
  struct dat_log_entry *e = &dat_log[h % DAT_LOG_SIZE];
  e->key = key;
  e->event = event;
  e->popped = (uint8_t)popped; // 0: real register, 1: same key read again, 2: new key latched
  e->icr = __atomic_load_n(&remote_diag_icr, __ATOMIC_RELAXED);
  e->dat = __atomic_load_n(&remote_diag_dat, __ATOMIC_RELAXED);
  e->acks = __atomic_load_n(&remote_diag_intreq, __ATOMIC_RELAXED);
  __atomic_store_n(&dat_log_head, h + 1, __ATOMIC_RELEASE);
}

static void print_dat_log(void) {
  uint32_t head = __atomic_load_n(&dat_log_head, __ATOMIC_ACQUIRE);
  if (head - dat_log_printed > DAT_LOG_SIZE)
    dat_log_printed = head - DAT_LOG_SIZE; // overwritten entries are lost
  while (dat_log_printed != head) {
    struct dat_log_entry *e = &dat_log[dat_log_printed % DAT_LOG_SIZE];
    printf("[REMOTE] Amiga read CIAADAT (%s): key=0x%02x event=%u | icr_sp_reports=%u dat_reads=%u portsacks=%u\n",
           e->popped == 2 ? "new key" : (e->popped == 1 ? "same key again" : "not intercepted"), e->key, e->event,
           e->icr, e->dat, e->acks);
    dat_log_printed++;
  }
}

// Diagnostics: a key that stays down on our side for a while means the client's release is late or lost
// (as opposed to the Amiga reading it late, which watch_queue reports).
static void watch_held_keys(void) {
  int64_t now = 0;
  for (int i = 0; i < 256; i++) {
    if (!key_down[i] || key_held_reported[i])
      continue;
    if (!now)
      now = now_msec();
    if (now - key_down_at[i] >= KEY_HELD_MSEC) {
      key_held_reported[i] = 1;
      printf("[REMOTE] key %d down for %d ms, no release received from the client yet\n", i, KEY_HELD_MSEC);
    }
  }
}

// Diagnostics for the Amiga freezes: when the Amiga keeps not reading the keys we queued, log the state of the
// interrupt emulation once per episode. Read only, it changes nothing.
static void watch_queue(void) {
  static int64_t nonempty_since = 0;
  static int reported = 0;
  static uint32_t snap_icr, snap_dat, snap_intreq;
  int queued = get_num_kb_queued();

  if (!queued) {
    nonempty_since = 0;
    reported = 0;
    return;
  }
  int64_t now = now_msec();
  if (!nonempty_since) {
    nonempty_since = now;
    snap_icr = __atomic_load_n(&remote_diag_icr, __ATOMIC_RELAXED);
    snap_dat = __atomic_load_n(&remote_diag_dat, __ATOMIC_RELAXED);
    snap_intreq = __atomic_load_n(&remote_diag_intreq, __ATOMIC_RELAXED);
  }
  if (!reported && now - nonempty_since >= STUCK_QUEUE_MSEC) {
    reported = 1;
    printf("[REMOTE] queue stuck for %d ms: queued=%d emulated_irqs=0x%04x emulated_ipl=%u last_irq=%u irq=%d\n",
           STUCK_QUEUE_MSEC, queued, (unsigned)emulated_irqs, (unsigned)amiga_emulated_ipl(), (unsigned)last_irq, irq);
    // what the Amiga did since the queue became non-empty: ICR reads answered 0x88, CIAADAT pops, PORTS acknowledged
    // (INTREQ writes). SR/PC are read from another thread, so they are only a hint.
    printf("[REMOTE]   since then: icr=%u dat=%u intreq_ports=%u | last_last_irq=%u ipl_enabled[2]=%u cpu SR=%04x PC=%08x\n",
           (unsigned)(__atomic_load_n(&remote_diag_icr, __ATOMIC_RELAXED) - snap_icr),
           (unsigned)(__atomic_load_n(&remote_diag_dat, __ATOMIC_RELAXED) - snap_dat),
           (unsigned)(__atomic_load_n(&remote_diag_intreq, __ATOMIC_RELAXED) - snap_intreq),
           (unsigned)last_last_irq, (unsigned)ipl_enabled[2],
           m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_PC));
  }
}

// Never touch the interrupt state from here: ask the CPU thread, which is the only one raising PORTS
static void kick_keyboard_irq(void) {
  kb_request_irq();
}

static void try_release(int code) {
  if (queue_remote_keypress(code, KEYPRESS_RELEASE, PLATFORM_AMIGA)) {
    key_down[code] = 0;
    if (key_release_pending[code]) {
      key_release_pending[code] = 0;
      pending_releases--;
    }
    kick_keyboard_irq();
  } else if (!key_release_pending[code]) {
    // key queue is full; keep trying so the key doesn't get stuck down on the Amiga
    key_release_pending[code] = 1;
    pending_releases++;
  }
}

static void flush_pending_releases(void) {
  if (!pending_releases)
    return;
  for (int i = 0; i < 256; i++) {
    if (key_release_pending[i])
      try_release(i);
  }
}

static void handle_key(int code, int pressed) {
  if (code < 0 || code > 255)
    return;

  if (code == KEY_CAPSLOCK) {
    // the Amiga caps lock key is a latching key: every toggle is one "press" event
    if (pressed && queue_remote_keypress(code, KEYPRESS_PRESS, PLATFORM_AMIGA))
      kick_keyboard_irq();
    return;
  }

  if (pressed) {
    if (key_down[code])
      return;
    if (queue_remote_keypress(code, KEYPRESS_PRESS, PLATFORM_AMIGA)) {
      key_down[code] = 1;
      key_held_reported[code] = 0;
      key_down_at[code] = now_msec();
      kick_keyboard_irq();
    }
  } else if (key_down[code]) {
    if (key_held_reported[code])
      printf("[REMOTE] key %d released after %d ms\n", code, (int)(now_msec() - key_down_at[code]));
    try_release(code);
  }
}

static void release_everything(void) {
  for (int i = 0; i < 256; i++) {
    if (key_down[i])
      try_release(i);
  }
  clear_injected_mouse();
  clear_injected_joystick();
}

static void handle_line(char *line) {
  int a, b;
  switch (line[0]) {
    case 'K':
      if (sscanf(line + 1, "%d %d", &a, &b) == 2)
        handle_key(a, b != 0);
      break;
    case 'M':
      if (sscanf(line + 1, "%d %d", &a, &b) == 2)
        inject_mouse_move(a, b);
      break;
    case 'B':
      if (sscanf(line + 1, "%d", &a) == 1)
        inject_mouse_buttons((uint8_t)a);
      break;
    case 'J':
      if (sscanf(line + 1, "%d", &a) == 1)
        inject_joystick((uint8_t)a);
      break;
    case 'W':
      // newmouse wheel up/down are the pseudo keys 0xfe/0xff in keymap_amiga, press only (as for the local mouse)
      if (sscanf(line + 1, "%d", &a) == 1) {
        int n = a < 0 ? -a : a;
        if (n > 5)
          n = 5;
        for (int i = 0; i < n; i++) {
          if (!queue_remote_keypress(a > 0 ? 0xfe : 0xff, KEYPRESS_PRESS, PLATFORM_AMIGA))
            break;
          kick_keyboard_irq();
        }
      }
      break;
    case 'P':
      if (client_fd != -1)
        (void)!write(client_fd, "PONG\n", 5);
      break;
    default:
      break;
  }
}

static void drop_client(void) {
  if (client_fd == -1)
    return;
  close(client_fd);
  client_fd = -1;
  release_everything();
  printf("[REMOTE] Client disconnected\n");
}

static void accept_client(void) {
  int fd = accept(listen_fd, NULL, NULL);
  if (fd == -1)
    return;

  // a new client replaces a previous one; this also recovers from half-dead SSH tunnels
  drop_client();

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  client_fd = fd;

  // The mouse needs no hook: the remote offset is added to the real counters. Keys go through the key queue,
  // which the CIA reads serve only while remote_kb_active is set; the Amiga's own keyboard keeps working
  // when nothing is queued.
  remote_kb_active = 1;
  printf("[REMOTE] Client connected\n");
}

static void read_client(void) {
  static char buf[LINE_MAX_LEN];
  static size_t len = 0;
  char tmp[512];

  ssize_t n = read(client_fd, tmp, sizeof(tmp));
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    len = 0;
    drop_client();
    return;
  }

  for (ssize_t i = 0; i < n; i++) {
    char c = tmp[i];
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = 0;
        handle_line(buf);
        len = 0;
      }
    } else if (len < LINE_MAX_LEN - 1) {
      buf[len++] = c;
    } else {
      len = 0; // overlong line, discard
    }
  }
}

static void *remote_task(void *arg) {
  (void)arg;

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd == -1) {
    printf("[REMOTE] Cannot create socket: %s\n", strerror(errno));
    return NULL;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: reach it through an SSH tunnel
  addr.sin_port = htons(remote_port);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(listen_fd, 1) == -1) {
    printf("[REMOTE] Cannot listen on 127.0.0.1:%u: %s\n", remote_port, strerror(errno));
    close(listen_fd);
    listen_fd = -1;
    return NULL;
  }
  printf("[REMOTE] Listening on 127.0.0.1:%u\n", remote_port);

  while (!__atomic_load_n(&remote_stop, __ATOMIC_RELAXED)) {
    struct pollfd fds[2];
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    fds[1].fd = client_fd; // -1 is ignored by poll
    fds[1].events = POLLIN;

    int busy = pending_releases || get_num_kb_queued();
    int rc = poll(fds, 2, busy ? RETRY_INTERVAL_MSEC : IDLE_INTERVAL_MSEC);

    if (rc > 0) {
      if (fds[0].revents & POLLIN)
        accept_client();
      else if (client_fd != -1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
        read_client();
    }

    // a keypress is only delivered if the Amiga services the interrupt; keep asking until the queue drains
    flush_pending_releases();
    watch_queue();
    watch_held_keys();
    print_dat_log();
    if (kb_irq_wanted())
      kick_keyboard_irq();
    else if (client_fd == -1 && !pending_releases)
      remote_kb_active = 0; // queue drained after the last client left, hand the CIA back
  }

  drop_client();
  close(listen_fd);
  listen_fd = -1;
  return NULL;
}

int remote_input_start(unsigned int port) {
  if (remote_running || port == 0 || port > 65535)
    return -1;

  remote_port = port;
  __atomic_store_n(&remote_stop, 0, __ATOMIC_RELAXED);
  memset(key_down, 0, sizeof(key_down));
  memset(key_release_pending, 0, sizeof(key_release_pending));
  pending_releases = 0;

  int err = pthread_create(&remote_tid, NULL, remote_task, NULL);
  if (err != 0) {
    printf("[ERROR] Cannot create remote input thread: [%s]\n", strerror(err));
    return -1;
  }
  pthread_setname_np(remote_tid, "pistorm: remote");
  remote_running = 1;
  return 0;
}

void remote_input_stop(void) {
  if (!remote_running)
    return;
  __atomic_store_n(&remote_stop, 1, __ATOMIC_RELAXED);
  pthread_join(remote_tid, NULL);
  remote_running = 0;
}
