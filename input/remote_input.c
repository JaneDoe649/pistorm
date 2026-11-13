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
#include <unistd.h>

#include "input/input.h"
#include "input/remote_input.h"
#include "platforms/amiga/amiga-interrupts.h"
#include "platforms/platforms.h"

// defined in emulator.c
extern int kb_hook_enabled, mouse_hook_enabled;
extern uint32_t last_irq;

#define LINE_MAX_LEN 128
// how often the thread wakes up to retry queued releases / re-raise the keyboard interrupt
#define RETRY_INTERVAL_MSEC 5
#define IDLE_INTERVAL_MSEC 200

static pthread_t remote_tid;
static int remote_running = 0;
static volatile int remote_stop = 0;
static unsigned int remote_port = 0;

static int listen_fd = -1, client_fd = -1;
static uint8_t key_down[256], key_release_pending[256];
static int pending_releases = 0;

static void kick_keyboard_irq(void) {
  // same condition as keyboard_task in emulator.c
  if (last_irq != 2)
    amiga_emulate_irq(PORTS);
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
      kick_keyboard_irq();
    }
  } else if (key_down[code]) {
    try_release(code);
  }
}

static void release_everything(void) {
  for (int i = 0; i < 256; i++) {
    if (key_down[i])
      try_release(i);
  }
  clear_injected_mouse();
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
    case 'W':
      if (sscanf(line + 1, "%d", &a) == 1)
        inject_mouse_wheel(a);
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

  // remote input is useless unless the hooks are on, so a connection switches them on
  kb_hook_enabled = 1;
  mouse_hook_enabled = 1;
  printf("[REMOTE] Client connected, keyboard and mouse hooks enabled\n");
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
    if (get_num_kb_queued())
      kick_keyboard_irq();
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
