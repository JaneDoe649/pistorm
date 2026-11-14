// SPDX-License-Identifier: MIT

// Diagnostics for the remote input, see remote_diag.h. Nothing is built without REMOTE_DIAG.
#ifdef REMOTE_DIAG

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config_file/config_file.h"
#include "gpio/ps_protocol.h"
#include "input/input.h"
#include "input/remote_diag.h"
#include "m68k.h"
#include "platforms/amiga/amiga-interrupts.h"
#include "platforms/amiga/amiga-registers.h"

// defined in emulator.c
extern uint32_t last_irq;
extern int irq;                // set by ipl_task: an interrupt is pending, the CPU then runs in tiny slices
extern uint16_t emulated_irqs; // platforms/amiga/amiga-interrupts.c
extern uint32_t last_last_irq;
extern uint8_t ipl_enabled[8];

#define STUCK_QUEUE_MSEC 800

uint32_t remote_diag_icr = 0, remote_diag_dat = 0, remote_diag_intreq = 0;
uint32_t remote_diag_joydat[2][3], remote_diag_pra = 0;
int32_t remote_diag_pra_type = -1, remote_diag_pra_fire = -1, remote_diag_ddra = -1, remote_diag_potgo = -1;

static int64_t now_msec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// every CIA / custom register read (address, width) while the remote joystick fire is down, to see which one a game
// really uses to read the button. Filled by the CPU thread, printed and cleared by the remote input thread.
#define FIRE_CENSUS_SIZE 64
struct fire_census_entry { uint32_t addr, count; uint8_t type; };
static struct fire_census_entry fire_census[FIRE_CENSUS_SIZE];

void remote_diag_fire_census_hit(uint8_t type, uint32_t addr) {
  for (int i = 0; i < FIRE_CENSUS_SIZE; i++) {
    struct fire_census_entry *e = &fire_census[i];
    if (e->count && e->addr == addr && e->type == type) {
      __atomic_fetch_add(&e->count, 1, __ATOMIC_RELAXED);
      return;
    }
    if (!e->count) {
      e->addr = addr;
      e->type = type;
      __atomic_store_n(&e->count, 1, __ATOMIC_RELEASE);
      return;
    }
  }
}

// Registers read by the Amiga while the joystick fire was down, printed when the fire is released
static void print_fire_census(void) {
  static const char *const widths[] = {"byte", "word", "long"};
  printf("[REMOTE] registers read while fire was down:\n");
  for (int i = 0; i < FIRE_CENSUS_SIZE; i++) {
    struct fire_census_entry *e = &fire_census[i];
    uint32_t n = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
    if (!n)
      break;
    printf("[REMOTE]   0x%06x %-4s x%u\n", (unsigned)e->addr, e->type < 3 ? widths[e->type] : "?", (unsigned)n);
    __atomic_store_n(&e->count, 0, __ATOMIC_RELAXED);
  }
}

void remote_diag_joystick(int mask) {
  static int fire_was_down = 0;
  int fire_down = (mask & 0x10) != 0;
  if (fire_was_down && !fire_down)
    print_fire_census();
  fire_was_down = fire_down;
  printf("[REMOTE] joystick mask %d (1 up, 2 down, 4 left, 8 right, 16 fire, 32 button 2)\n", mask & 0x3F);
}

// last writes to INTENA (width and value as seen by the hook): our shadow of the master enable is rebuilt from them
#define INTENA_LOG_SIZE 8
struct intena_log_entry { uint8_t type; uint16_t addr_lo; uint32_t val; };
static struct intena_log_entry intena_log[INTENA_LOG_SIZE];
static uint32_t intena_count = 0;

void remote_diag_log_intena(uint8_t type, uint32_t addr, uint32_t val) {
  uint32_t n = intena_count;
  intena_log[n % INTENA_LOG_SIZE] = (struct intena_log_entry){type, (uint16_t)(addr & 0xFFFF), val};
  __atomic_store_n(&intena_count, n + 1, __ATOMIC_RELEASE);
}

// memory dump requested by the remote input thread when the queue is stuck: only the CPU thread may touch the bus.
// Also copied: the level 2 handler (autovector at VBR 0, assumed 0; a JMP.L is followed), and the real INTENAR /
// INTREQR, to compare with our shadow of the interrupt enables.
#define CODE_DUMP_SIZE 256
static uint32_t dump_addr = 0;
static int dump_req = 0, dump_done = 0;
static uint8_t dump_buf[CODE_DUMP_SIZE];
static uint32_t l2_vector = 0, l2_target = 0;
static uint8_t l2_buf[CODE_DUMP_SIZE];
static uint16_t real_intenar = 0, real_intreqr = 0;

void remote_diag_cpu_poll(void) {
  if (!__atomic_load_n(&dump_req, __ATOMIC_ACQUIRE))
    return;
  for (int i = 0; i < CODE_DUMP_SIZE; i++)
    dump_buf[i] = (uint8_t)m68k_read_memory_8(dump_addr + i);
  l2_vector = m68k_read_memory_32(0x68);
  l2_target = l2_vector;
  if (m68k_read_memory_16(l2_target) == 0x4ef9)
    l2_target = m68k_read_memory_32(l2_target + 2);
  for (int i = 0; i < CODE_DUMP_SIZE; i++)
    l2_buf[i] = (uint8_t)m68k_read_memory_8(l2_target + i);
  real_intenar = (uint16_t)ps_read_16(INTENAR);
  real_intreqr = (uint16_t)ps_read_16(INTREQR);
  __atomic_store_n(&dump_req, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&dump_done, 1, __ATOMIC_RELEASE);
}

// Where the CPU spends the stuck window: PC/SR sampled from the remote thread (racy but good enough for a histogram),
// and the memory around the most frequent PC, copied by the CPU thread.
#define PC_SAMPLES 40
static uint32_t pc_samples[PC_SAMPLES], sr_samples[PC_SAMPLES];
static int n_pc_samples = 0;
static uint32_t dump_base = 0;
static int dump_pending = 0;

static void sample_pc(void) {
  static int64_t last = 0;
  int64_t now = now_msec();
  if (n_pc_samples >= PC_SAMPLES || now - last < 15)
    return;
  last = now;
  pc_samples[n_pc_samples] = m68k_get_reg(NULL, M68K_REG_PC);
  sr_samples[n_pc_samples++] = m68k_get_reg(NULL, M68K_REG_SR);
}

static void report_pc_histogram(void) {
  printf("[REMOTE]   PC samples (%d):", n_pc_samples);
  uint32_t best = 0;
  int best_n = 0;
  for (int i = 0; i < n_pc_samples; i++) {
    int n = 0, seen = 0;
    for (int j = 0; j < n_pc_samples; j++)
      if (pc_samples[j] == pc_samples[i])
        n++;
    for (int j = 0; j < i; j++)
      if (pc_samples[j] == pc_samples[i])
        seen = 1;
    if (!seen)
      printf(" %08x(SR=%04x)x%d", pc_samples[i], sr_samples[i], n);
    if (n > best_n) {
      best_n = n;
      best = pc_samples[i];
    }
  }
  printf("\n");
  if (best_n) {
    dump_base = (best & ~1u) - 0x80;
    dump_addr = dump_base;
    __atomic_store_n(&dump_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dump_req, 1, __ATOMIC_RELEASE);
    dump_pending = 1;
  }
}

static void print_code_dump(void) {
  if (!dump_pending || !__atomic_load_n(&dump_done, __ATOMIC_ACQUIRE))
    return;
  dump_pending = 0;
  printf("[REMOTE]   memory around the busiest PC:\n");
  for (int i = 0; i < CODE_DUMP_SIZE; i += 16) {
    printf("[REMOTE]   %08x:", dump_base + i);
    for (int k = 0; k < 16; k += 2)
      printf(" %02x%02x", dump_buf[i + k], dump_buf[i + k + 1]);
    printf("\n");
  }
  printf("[REMOTE]   real INTENAR=%04x INTREQR=%04x | our shadow: INTEN=%u PORTS=%u | level 2 vector=%08x -> %08x:\n",
         (unsigned)real_intenar, (unsigned)real_intreqr, (unsigned)ipl_enabled[7], (unsigned)ipl_enabled[2],
         (unsigned)l2_vector, (unsigned)l2_target);
  for (int i = 0; i < CODE_DUMP_SIZE; i += 16) {
    printf("[REMOTE]   %08x:", l2_target + i);
    for (int k = 0; k < 16; k += 2)
      printf(" %02x%02x", l2_buf[i + k], l2_buf[i + k + 1]);
    printf("\n");
  }
}

// Reads of CIAADAT by the Amiga, recorded by the CPU thread (no printf there) and printed by this thread.
// Each read pops one queued key, whereas the real register can be read any number of times: two readers
// (or a handler reading twice) would swallow events, which is what this log is meant to show.
#define DAT_LOG_SIZE 64
struct dat_log_entry { uint8_t key, event, popped; uint32_t icr, dat, acks, timeouts; int64_t ms; };
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
  e->timeouts = __atomic_load_n(&kb_sdr_timeouts, __ATOMIC_RELAXED); // handshakes that never came (150 ms fallback)
  e->ms = now_msec();
  __atomic_store_n(&dat_log_head, h + 1, __ATOMIC_RELEASE);
}

static void print_dat_log(void) {
  uint32_t head = __atomic_load_n(&dat_log_head, __ATOMIC_ACQUIRE);
  if (head - dat_log_printed > DAT_LOG_SIZE)
    dat_log_printed = head - DAT_LOG_SIZE; // overwritten entries are lost
  // Reads answered by the real register (nothing queued) can come in thousands when a game polls CIAADAT:
  // print the first one of a run and summarize the rest, so they do not bury the lines that matter.
  static uint32_t real_run = 0;
  while (dat_log_printed != head) {
    struct dat_log_entry *e = &dat_log[dat_log_printed % DAT_LOG_SIZE];
    if (!e->popped) {
      if (real_run++) {
        dat_log_printed++;
        continue;
      }
    } else if (real_run > 1) {
      printf("[REMOTE]   ... %u more reads answered by the real register\n", (unsigned)(real_run - 1));
      real_run = 0;
    } else {
      real_run = 0;
    }
    printf("[REMOTE] %lld ms Amiga read CIAADAT (%s): key=0x%02x event=%u | icr_sp_reports=%u dat_reads=%u portsacks=%u handshake_timeouts=%u\n",
           (long long)(e->ms % 1000000), e->popped == 2 ? "new key" : (e->popped == 1 ? "same key again" : "not intercepted"),
           e->key, e->event, e->icr, e->dat, e->acks, e->timeouts);
    dat_log_printed++;
  }
}

// Diagnostics: how the game reads the joystick registers (width of the accesses) while the remote joystick is active,
// printed when the counters changed, at most once a second.
static void watch_joystick_reads(void) {
  static int64_t last = 0;
  static uint32_t prev[7];
  int64_t now = now_msec();
  if (now - last < 1000)
    return;
  uint32_t cur[7];
  for (int i = 0; i < 6; i++)
    cur[i] = __atomic_load_n(&remote_diag_joydat[i / 3][i % 3], __ATOMIC_RELAXED);
  cur[6] = __atomic_load_n(&remote_diag_pra, __ATOMIC_RELAXED);
  if (!memcmp(cur, prev, sizeof(cur)))
    return;
  last = now;
  memcpy(prev, cur, sizeof(cur));
  printf("[REMOTE] joystick reads: JOY0DAT b=%u w=%u l=%u | JOY1DAT b=%u w=%u l=%u | CIAAPRA=%u\n", cur[0], cur[1], cur[2],
         cur[3], cur[4], cur[5], cur[6]);
  printf("[REMOTE]   CIAAPRA last width=%d (0 byte, 1 word, 2 long) value shown with fire down=0x%02x | "
         "last DDRA write=0x%02x last POTGO write=0x%04x (-1: none)\n",
         (int)remote_diag_pra_type, (unsigned)remote_diag_pra_fire & 0xFFFFFFFFu, (unsigned)remote_diag_ddra,
         (unsigned)remote_diag_potgo);
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
    n_pc_samples = 0;
    return;
  }
  sample_pc();
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
    // our shadow of INTENA: master enable (bit 14) and the last writes the hook saw, newest first
    printf("[REMOTE]   INTEN master=%u, %u INTENA writes, last:", (unsigned)ipl_enabled[7],
           (unsigned)__atomic_load_n(&intena_count, __ATOMIC_ACQUIRE));
    uint32_t cnt = __atomic_load_n(&intena_count, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < 6 && k < cnt; k++) {
      struct intena_log_entry *e = &intena_log[(cnt - 1 - k) % INTENA_LOG_SIZE];
      printf(" %s@%04x=0x%08x", e->type == 0 ? "b" : (e->type == 1 ? "w" : "l"), (unsigned)e->addr_lo, (unsigned)e->val);
    }
    printf("\n");
    report_pc_histogram();
  }
}

void remote_diag_poll(void) {
  watch_queue();
  watch_joystick_reads();
  print_code_dump();
  print_dat_log();
}

#endif // REMOTE_DIAG

typedef int remote_diag_translation_unit_is_not_empty;
