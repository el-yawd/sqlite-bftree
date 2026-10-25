/*
** Minimal shim for ESBMC verification of BF-Tree pure modules: the SQLite
** typedefs and allocator seams that src/bf_wal.c needs to compile standalone.
*/
#ifndef BF_SHIM_H
#define BF_SHIM_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long long i64;
typedef unsigned long long u64;

#define sqlite3_malloc(n)    malloc(n)
#define sqlite3_malloc64(n)  malloc(n)
#define sqlite3_realloc(p,n) realloc(p,n)
#define sqlite3_free(p)      free(p)
#define SQLITE_PRIVATE       static

#ifdef SQLITE_OMIT_BF_CACHE
# undef SQLITE_OMIT_BF_CACHE
#endif

/* Source-level asserts become proof obligations, never no-ops: an invariant
** the code states is exactly what a model checker should check. */
#undef assert
#define assert(x) __ESBMC_assert((x), "source assert: " #x)

/* ESBMC's native nondeterministic-value functions (no body: any value). */
int __VERIFIER_nondet_int(void);
unsigned int __VERIFIER_nondet_uint(void);
unsigned char __VERIFIER_nondet_uchar(void);
long __VERIFIER_nondet_long(void);

#endif /* BF_SHIM_H */
