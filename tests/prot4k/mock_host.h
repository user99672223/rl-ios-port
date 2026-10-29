/*
 * Mock host backend: simulates a host with 'host_page_size' pages (e.g. 16 KB) on top of the
 * 4 KB-page Debian machine.  It records every call, tracks the protection of every host page,
 * keeps real backing bytes (to test zero-fill) and flags any violation of the host rules:
 *   - an address or length that is not host-page aligned,
 *   - a protection change on an unmapped page,
 *   - W|X ever requested (iOS/macOS W^X),
 *   - a zero() on a page that is not host-writable.
 */
#ifndef MOCK_HOST_H
#define MOCK_HOST_H

#include <stdint.h>
#include <stddef.h>
#include "prot4k_space.h"

#define MOCK_MAX_CALLS 4096

struct mock_call
{
    int       kind;     /* MC_* */
    uintptr_t addr;
    size_t    len;
    int       prot;
};
enum { MC_MAP, MC_UNMAP, MC_DISCARD, MC_MPROTECT, MC_ZERO };

struct mock_host
{
    uintptr_t base;             /* modelled address of mem[0] */
    size_t    size;
    uintptr_t hps;              /* host page size */
    uint8_t  *mem;
    uint8_t  *mapped;           /* per host page */
    int      *prot;             /* per host page */
    int       check_wx;         /* flag W|X as a violation */
    unsigned  violations;
    char      first_violation[160];
    struct mock_call calls[MOCK_MAX_CALLS];
    unsigned  ncalls;           /* total since last reset (calls[] keeps the first MOCK_MAX_CALLS) */
    unsigned  n_mprotect;
    unsigned  conflicts;        /* W^X conflicts reported through the ops->conflict hook */
};

struct mock_host *mock_new( uintptr_t base, size_t size, uintptr_t host_page_size, int check_wx );
void mock_free( struct mock_host *m );
void mock_reset_calls( struct mock_host *m );
p4k_space_host mock_space_host( struct mock_host *m );

/* host-level access helpers used by the tests; they return 0 and record a violation when the
 * host protection would have faulted */
int  mock_prot_at( struct mock_host *m, uintptr_t addr );   /* -1 if unmapped */
int  mock_write8( struct mock_host *m, uintptr_t addr, uint8_t v );
int  mock_read8( struct mock_host *m, uintptr_t addr, uint8_t *v );
/* same, but never records a violation (for probing) */
int  mock_can( struct mock_host *m, uintptr_t addr, int need );

#endif
