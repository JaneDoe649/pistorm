// SPDX-License-Identifier: MIT
//
// A2088Test: bus diagnostics for a Zorro II card (made for the A2088 Bridgeboard) on a PiStorm.
//
//   A2088Test LIST
//       List every autoconfig board (manufacturer, product, base, size).
//   A2088Test ODD
//       Check unaligned (odd address) byte/word/long accesses on chip RAM. Does not touch the card.
//   A2088Test RAM <mfg> <prod> [offset size passes] [WRITE]
//       Read stability test of the board region (reads it twice and compares). Read only unless
//       WRITE is given; with WRITE, patterns are written with byte/word/long cycles and read back
//       with other widths. Quit PC-Mono / Janus first: WRITE overwrites the region.
//       The A2088 is manufacturer 513, product 1 (512 KB at $200000 on the tested machine, the
//       Janus shared RAM). Use LIST to confirm. Numbers can be decimal or 0x hex.
//
// Build (68020 code is required to emit odd address accesses):
//   m68k-amigaos-gcc a2088test.c -m68020 -O2 -o A2088Test -noixemul -Wall

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ExpansionBase *ExpansionBase = NULL;

#define MAX_REPORT 16

static unsigned long parse_num(const char *s) {
    return strtoul(s, NULL, 0);
}

static int cmd_list(void) {
    struct ConfigDev *cd = NULL;
    int n = 0;

    printf("Mfg   Prod  Base       Size       Flags\n");
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        printf("%-5u %-5u $%08lX $%08lX $%02X\n",
               (unsigned)cd->cd_Rom.er_Manufacturer, (unsigned)cd->cd_Rom.er_Product,
               (unsigned long)cd->cd_BoardAddr, (unsigned long)cd->cd_BoardSize,
               (unsigned)cd->cd_Rom.er_Flags);
        n++;
    }
    printf("%d board(s).\n", n);
    return 0;
}

static int check_byte(const char *what, int idx, volatile UBYTE *p, UBYTE expected) {
    if (p[idx] != expected) {
        printf("  FAIL %s: byte %d is $%02X, expected $%02X\n", what, idx, p[idx], expected);
        return 1;
    }
    return 0;
}

static int cmd_odd(void) {
    volatile UBYTE *p = AllocMem(64, MEMF_CHIP | MEMF_CLEAR);
    int i, errors = 0;
    UWORD w;
    ULONG l;

    if (!p) {
        printf("Could not allocate chip RAM.\n");
        return 1;
    }
    printf("Unaligned accesses on chip RAM at $%08lX (real bus path):\n", (unsigned long)p);

    // Odd word write: $1234 at p+1 must give p[1]=$12, p[2]=$34.
    for (i = 0; i < 16; i++) p[i] = 0xEE;
    *(volatile UWORD *)(p + 1) = 0x1234;
    errors += check_byte("odd word write", 0, p, 0xEE);
    errors += check_byte("odd word write", 1, p, 0x12);
    errors += check_byte("odd word write", 2, p, 0x34);
    errors += check_byte("odd word write", 3, p, 0xEE);

    // Odd long write: $11223344 at p+5 must give 11 22 33 44.
    for (i = 0; i < 16; i++) p[i] = 0xEE;
    *(volatile ULONG *)(p + 5) = 0x11223344UL;
    errors += check_byte("odd long write", 4, p, 0xEE);
    errors += check_byte("odd long write", 5, p, 0x11);
    errors += check_byte("odd long write", 6, p, 0x22);
    errors += check_byte("odd long write", 7, p, 0x33);
    errors += check_byte("odd long write", 8, p, 0x44);
    errors += check_byte("odd long write", 9, p, 0xEE);

    // Odd word read.
    p[9] = 0xAB;
    p[10] = 0xCD;
    w = *(volatile UWORD *)(p + 9);
    if (w != 0xABCD) {
        printf("  FAIL odd word read: got $%04X, expected $ABCD\n", w);
        errors++;
    }

    // Odd long read.
    p[1] = 0x01; p[2] = 0x02; p[3] = 0x03; p[4] = 0x04;
    l = *(volatile ULONG *)(p + 1);
    if (l != 0x01020304UL) {
        printf("  FAIL odd long read: got $%08lX, expected $01020304\n", (unsigned long)l);
        errors++;
    }

    FreeMem((APTR)p, 64);
    printf(errors ? "%d error(s).\n" : "No error.\n", errors);
    return errors != 0;
}

static void write_region(volatile UBYTE *dst, const UBYTE *src, ULONG n, int width) {
    ULONG i;
    if (width == 8) {
        for (i = 0; i < n; i++) dst[i] = src[i];
    } else if (width == 16) {
        for (i = 0; i < n; i += 2) *(volatile UWORD *)(dst + i) = *(const UWORD *)(src + i);
    } else {
        for (i = 0; i < n; i += 4) *(volatile ULONG *)(dst + i) = *(const ULONG *)(src + i);
    }
}

static void read_region(UBYTE *dst, const volatile UBYTE *src, ULONG n, int width) {
    ULONG i;
    if (width == 8) {
        for (i = 0; i < n; i++) dst[i] = src[i];
    } else if (width == 16) {
        for (i = 0; i < n; i += 2) *(UWORD *)(dst + i) = *(const volatile UWORD *)(src + i);
    } else {
        for (i = 0; i < n; i += 4) *(ULONG *)(dst + i) = *(const volatile ULONG *)(src + i);
    }
}

static int popcount8(UBYTE v) {
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

// Compare two buffers, print the first mismatches and a lane/bit summary. Returns the error count.
static ULONG compare(const char *what, const UBYTE *exp, const UBYTE *got, ULONG n, ULONG base_off) {
    ULONG i, errors = 0, even = 0, odd = 0, single = 0;

    for (i = 0; i < n; i++) {
        if (exp[i] == got[i]) continue;
        if (errors < MAX_REPORT)
            printf("  %s: offset $%lX expected $%02X got $%02X\n", what,
                   (unsigned long)(base_off + i), exp[i], got[i]);
        errors++;
        if (i & 1) odd++; else even++;
        if (popcount8(exp[i] ^ got[i]) == 1) single++;
    }
    if (errors)
        printf("  %s: %lu byte error(s) (even lane/UDS %lu, odd lane/LDS %lu, single-bit %lu)\n",
               what, (unsigned long)errors, (unsigned long)even, (unsigned long)odd,
               (unsigned long)single);
    return errors;
}

static int cmd_ram(int argc, char **argv) {
    static const int wr_width[] = { 8, 16, 32, 16, 32 };
    static const int rd_width[] = { 16, 8, 32, 32, 16 };
    struct ConfigDev *cd;
    volatile UBYTE *board;
    UBYTE *exp = NULL, *got = NULL, *got2 = NULL;
    ULONG offset = 0, size = 0x1000, passes = 100, total = 0, i, p;
    int write_mode = 0, rc = 0, ph;
    long mfg, prod;

    if (argc < 4) {
        printf("Usage: A2088Test RAM <mfg> <prod> [offset size passes] [WRITE]\n");
        return 1;
    }
    mfg = (long)parse_num(argv[2]);
    prod = (long)parse_num(argv[3]);
    if (argc > 4 && strcmp(argv[argc - 1], "WRITE") == 0) {
        write_mode = 1;
        argc--;
    }
    if (argc > 4) offset = parse_num(argv[4]);
    if (argc > 5) size = parse_num(argv[5]);
    if (argc > 6) passes = parse_num(argv[6]);

    offset &= ~3UL;
    size &= ~3UL;
    if (size == 0 || passes == 0) {
        printf("Size and passes must be non-zero.\n");
        return 1;
    }

    cd = FindConfigDev(NULL, mfg, prod);
    if (!cd) {
        printf("Board %ld/%ld not found. Use LIST.\n", mfg, prod);
        return 1;
    }
    if (offset + size > (ULONG)cd->cd_BoardSize) {
        printf("Region $%lX+$%lX exceeds the board size $%lX.\n", (unsigned long)offset,
               (unsigned long)size, (unsigned long)cd->cd_BoardSize);
        return 1;
    }
    board = (volatile UBYTE *)cd->cd_BoardAddr + offset;
    printf("Board %ld/%ld at $%08lX, testing $%lX bytes from offset $%lX, %lu pass(es), %s.\n",
           mfg, prod, (unsigned long)cd->cd_BoardAddr, (unsigned long)size,
           (unsigned long)offset, (unsigned long)passes,
           write_mode ? "WRITE mode" : "read only");

    exp = AllocMem(size, MEMF_ANY | MEMF_CLEAR);
    got = AllocMem(size, MEMF_ANY | MEMF_CLEAR);
    got2 = AllocMem(size, MEMF_ANY | MEMF_CLEAR);
    if (!exp || !got || !got2) {
        printf("Out of memory.\n");
        rc = 1;
        goto done;
    }

    for (p = 0; p < passes; p++) {
        if (!write_mode) {
            // Two reads of the same region must agree, whatever it contains. Skip volatile
            // regions yourself (PC running) or expect noise.
            read_region(got, board, size, 16);
            read_region(got2, board, size, 16);
            total += compare("read/read", got, got2, size, offset);
            continue;
        }
        // Address and pass dependent pattern: a lost or misplaced write shows up.
        for (i = 0; i < size; i++)
            exp[i] = (UBYTE)((i * 7 + (i >> 8) * 31 + p * 13 + 0x5A) ^ (i & 1 ? 0xA5 : 0x3C));
        for (ph = 0; ph < 5; ph++) {
            char what[24];
            sprintf(what, "w%d/r%d", wr_width[ph], rd_width[ph]);
            write_region(board, exp, size, wr_width[ph]);
            read_region(got, board, size, rd_width[ph]);
            total += compare(what, exp, got, size, offset);
        }
    }

    printf("Done: %lu byte error(s) in total over %lu pass(es).\n", (unsigned long)total,
           (unsigned long)passes);
    rc = total != 0;

done:
    if (exp) FreeMem(exp, size);
    if (got) FreeMem(got, size);
    if (got2) FreeMem(got2, size);
    return rc;
}

int main(int argc, char **argv) {
    int rc;

    if (argc < 2) {
        printf("Usage: A2088Test LIST | ODD | RAM <mfg> <prod> [offset size passes] [WRITE]\n");
        return 1;
    }
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) {
        printf("Failed to open expansion.library.\n");
        return 1;
    }
    if (strcmp(argv[1], "LIST") == 0 || strcmp(argv[1], "list") == 0) rc = cmd_list();
    else if (strcmp(argv[1], "ODD") == 0 || strcmp(argv[1], "odd") == 0) rc = cmd_odd();
    else if (strcmp(argv[1], "RAM") == 0 || strcmp(argv[1], "ram") == 0) rc = cmd_ram(argc, argv);
    else {
        printf("Unknown command %s.\n", argv[1]);
        rc = 1;
    }
    CloseLibrary((struct Library *)ExpansionBase);
    return rc;
}
