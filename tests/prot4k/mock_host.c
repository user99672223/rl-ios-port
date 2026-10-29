#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_host.h"

static void violation( struct mock_host *m, const char *fmt, uintptr_t a, size_t l, int p )
{
    if (!m->violations++) snprintf( m->first_violation, sizeof(m->first_violation), fmt,
                                    (unsigned long)a, (unsigned long)l, p );
}

static void record( struct mock_host *m, int kind, uintptr_t addr, size_t len, int prot )
{
    if (m->ncalls < MOCK_MAX_CALLS)
        m->calls[m->ncalls] = (struct mock_call){ kind, addr, len, prot };
    m->ncalls++;
}

static int check_range( struct mock_host *m, const char *what, uintptr_t addr, size_t len )
{
    char fmt[96];

    if ((addr & (m->hps - 1)) || (len & (m->hps - 1)) || !len)
    {
        snprintf( fmt, sizeof(fmt), "%s: unaligned/empty range %%lx+%%lx prot %%d", what );
        violation( m, fmt, addr, len, 0 );
        return -1;
    }
    if (addr < m->base || addr - m->base + len > m->size)
    {
        snprintf( fmt, sizeof(fmt), "%s: outside window %%lx+%%lx prot %%d", what );
        violation( m, fmt, addr, len, 0 );
        return -1;
    }
    return 0;
}

static size_t hp( struct mock_host *m, uintptr_t addr ) { return (addr - m->base) / m->hps; }

static int h_map( void *ud, uintptr_t addr, size_t len )
{
    struct mock_host *m = ud;
    size_t i;

    record( m, MC_MAP, addr, len, 0 );
    if (check_range( m, "map", addr, len )) return -1;
    for (i = 0; i < len / m->hps; i++)
        if (m->mapped[hp( m, addr ) + i]) { violation( m, "map: already mapped %lx+%lx %d", addr, len, 0 ); return -1; }
    for (i = 0; i < len / m->hps; i++) { m->mapped[hp( m, addr ) + i] = 1; m->prot[hp( m, addr ) + i] = 0; }
    memset( m->mem + (addr - m->base), 0, len );
    return 0;
}

static int h_unmap( void *ud, uintptr_t addr, size_t len )
{
    struct mock_host *m = ud;
    size_t i;

    record( m, MC_UNMAP, addr, len, 0 );
    if (check_range( m, "unmap", addr, len )) return -1;
    for (i = 0; i < len / m->hps; i++) { m->mapped[hp( m, addr ) + i] = 0; m->prot[hp( m, addr ) + i] = 0; }
    memset( m->mem + (addr - m->base), 0xdd, len );   /* poison: freed memory */
    return 0;
}

static int h_discard( void *ud, uintptr_t addr, size_t len )
{
    struct mock_host *m = ud;
    size_t i;

    record( m, MC_DISCARD, addr, len, 0 );
    if (check_range( m, "discard", addr, len )) return -1;
    for (i = 0; i < len / m->hps; i++)
    {
        if (!m->mapped[hp( m, addr ) + i]) { violation( m, "discard: unmapped %lx+%lx %d", addr, len, 0 ); return -1; }
        m->prot[hp( m, addr ) + i] = 0;
    }
    memset( m->mem + (addr - m->base), 0, len );
    return 0;
}

static int h_mprotect( void *ud, uintptr_t addr, size_t len, int prot )
{
    struct mock_host *m = ud;
    size_t i;

    record( m, MC_MPROTECT, addr, len, prot );
    m->n_mprotect++;
    if (check_range( m, "mprotect", addr, len )) return -1;
    if (m->check_wx && (prot & P4K_HOST_WRITE) && (prot & P4K_HOST_EXEC))
        violation( m, "mprotect: W|X requested %lx+%lx prot %d", addr, len, prot );
    for (i = 0; i < len / m->hps; i++)
    {
        if (!m->mapped[hp( m, addr ) + i]) { violation( m, "mprotect: unmapped %lx+%lx prot %d", addr, len, prot ); return -1; }
        m->prot[hp( m, addr ) + i] = prot;
    }
    return 0;
}

static int h_zero( void *ud, uintptr_t addr, size_t len )
{
    struct mock_host *m = ud;
    uintptr_t a;

    record( m, MC_ZERO, addr, len, 0 );
    if (addr < m->base || addr - m->base + len > m->size || (addr & 0xfff) || (len & 0xfff))
    {
        violation( m, "zero: bad range %lx+%lx %d", addr, len, 0 );
        return -1;
    }
    for (a = addr; a < addr + len; a += m->hps - (a & (m->hps - 1)) )
    {
        size_t h = hp( m, a );
        if (!m->mapped[h] || !(m->prot[h] & P4K_HOST_WRITE))
        {
            violation( m, "zero: host page not writable %lx+%lx prot %d", a, len, m->mapped[h] ? m->prot[h] : -1 );
            return -1;
        }
    }
    memset( m->mem + (addr - m->base), 0, len );
    return 0;
}

static void h_conflict( void *ud, uintptr_t addr )
{
    struct mock_host *m = ud;
    (void)addr;
    m->conflicts++;
}

struct mock_host *mock_new( uintptr_t base, size_t size, uintptr_t hps, int check_wx )
{
    struct mock_host *m = calloc( 1, sizeof(*m) );

    m->base = base; m->size = size; m->hps = hps; m->check_wx = check_wx;
    m->mem = calloc( 1, size );
    m->mapped = calloc( 1, size / hps );
    m->prot = calloc( size / hps, sizeof(int) );
    return m;
}

void mock_free( struct mock_host *m )
{
    free( m->mem ); free( m->mapped ); free( m->prot ); free( m );
}

void mock_reset_calls( struct mock_host *m )
{
    m->ncalls = 0;
    m->n_mprotect = 0;
}

p4k_space_host mock_space_host( struct mock_host *m )
{
    p4k_space_host h;

    memset( &h, 0, sizeof(h) );
    h.ops.mprotect = h_mprotect;
    h.ops.zero = h_zero;
    h.ops.conflict = h_conflict;
    h.ops.ud = m;
    h.map = h_map;
    h.unmap = h_unmap;
    h.discard = h_discard;
    return h;
}

int mock_prot_at( struct mock_host *m, uintptr_t addr )
{
    size_t h;

    if (addr < m->base || addr - m->base >= m->size) return -1;
    h = hp( m, addr );
    return m->mapped[h] ? m->prot[h] : -1;
}

int mock_can( struct mock_host *m, uintptr_t addr, int need )
{
    int p = mock_prot_at( m, addr );
    return p >= 0 && (p & need) == need;
}

int mock_write8( struct mock_host *m, uintptr_t addr, uint8_t v )
{
    if (!mock_can( m, addr, P4K_HOST_WRITE | P4K_HOST_READ ))
    {
        violation( m, "write8: host would fault at %lx+%lx prot %d", addr, 0, mock_prot_at( m, addr ) );
        return 0;
    }
    m->mem[addr - m->base] = v;
    return 1;
}

int mock_read8( struct mock_host *m, uintptr_t addr, uint8_t *v )
{
    if (!mock_can( m, addr, P4K_HOST_READ ))
    {
        violation( m, "read8: host would fault at %lx+%lx prot %d", addr, 0, mock_prot_at( m, addr ) );
        return 0;
    }
    *v = m->mem[addr - m->base];
    return 1;
}
