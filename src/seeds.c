#include "seeds.h"

const uint32_t SEED_BANK[SEED_COUNT] = {
    0x1337UL,       /* a: hacker classic, current default       */
    42UL,           /* b: Hitchhiker's Guide                    */
    0xDEADBEEFUL,   /* c: classic debug sentinel                */
    0xCAFEBABEUL,   /* d: Java .class magic number              */
    0xBADC0DEUL,    /* e: lightly self-deprecating              */
    0x8086UL        /* f: nod to the CPU we cross-compile for   */
};

const char *const SEED_LABEL[SEED_COUNT] = {
    "0x1337",
    "42",
    "0xDEADBEEF",
    "0xCAFEBABE",
    "0xBADC0DE",
    "0x8086"
};
