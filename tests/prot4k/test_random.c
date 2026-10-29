/*
 * Randomized test: thousands of random VirtualAlloc / VirtualProtect / VirtualFree /
 * VirtualQuery / image-map / write sequences are run against
 *   (a) the prot4k space on a mock 4/16/64 KB host, and
 *   (b) a simple reference model of Windows 4 KB semantics written here, independently of the
 *       library's internal encoding (it stores Win32 PAGE_* values per 4 KB page).
 *
 * After EVERY operation it asserts:
 *   1. same status as the model, same out-values (base, size, old protection);
 *   2. a full VirtualQuery walk of the window equals the model's regions, field by field;
 *   3. each host page is mapped iff an allocation covers it;
 *   4. the host protection of every host page equals the independently computed
 *      "most permissive union of its 4 KB parts, minus guest X, minus X on W^X conflict";
 *   5. the host never saw W|X, never an unaligned/unmapped call;
 *   6. no legitimate access would fault: every committed readable/writable page is
 *      host-readable/-writable (unless the policy deliberately arms a guard or write watch);
 *   7. memory contents: committed pages keep what was written; every uncommitted page inside
 *      an allocation reads zero (decommit zero-fill).
 * The first failure prints the config, seed, sequence and operation and exits non-zero.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_host.h"
#include "check.h"

int g_failures;

#define WBASE  ((uintptr_t)0x20000000)
#define NPG    1024u
#define WSIZE  ((size_t)NPG * 4096)
#define MAXA   128

/* ---------------------------------------------------------------- reference model ------ */
typedef struct { int alloc; int committed; uint32_t prot; int guard; int clean; int native; uint8_t data; } mpage;
typedef struct { int live; uintptr_t base; size_t size; uint32_t aprot; int image; int ww; } mall;
typedef struct { mpage pg[NPG]; mall al[MAXA]; } model_t;

static model_t M;
static struct
{
    unsigned long alloc_ok, commit_ok, protect_ok, decommit_ok, release_ok, image_ok, write_ok, ww_ok, errors;
    unsigned long mixed_host_pages;   /* host page whose 4 KB parts do not all have the same protection */
    unsigned long wx_union_pages;     /* host page whose union would be W|X before the rule is applied */
    unsigned long partial_decommit;   /* decommit that leaves a host page partly committed */
    unsigned long guard_pages, native_pages;
} stats;
static unsigned long long rng_state;
static unsigned rnd( void )
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 16);
}
static unsigned rn( unsigned n ) { return rnd() % n; }

static struct
{
    unsigned hps, flags;
    const char *name;
    unsigned long seed;
    unsigned seq, op;
    char desc[256];
} ctx;

static void fail( const char *fmt, ... ) __attribute__((noreturn, format(printf, 1, 2)));
static void fail( const char *fmt, ... )
{
    va_list ap;
    printf( "\nRANDOM TEST FAILURE\n  config=%s hps=%u flags=0x%x seed=%lu seq=%u op=%u\n  op: %s\n  ",
            ctx.name, ctx.hps, ctx.flags, ctx.seed, ctx.seq, ctx.op, ctx.desc );
    va_start( ap, fmt ); vprintf( fmt, ap ); va_end( ap );
    printf( "\n" );
    exit( 1 );
}

static int m_prot_valid( uint32_t p )
{
    switch (p & 0xff)
    {
    case 0x01: case 0x02: case 0x04: case 0x08: case 0x10: case 0x20: case 0x40: case 0x80: return 1;
    }
    return 0;
}
static int m_is_wc( uint32_t v ) { return v == P4K_PAGE_WRITECOPY || v == P4K_PAGE_EXECUTE_WRITECOPY; }
static uint32_t m_to_image( uint32_t v )
{
    if (v == P4K_PAGE_READWRITE) return P4K_PAGE_WRITECOPY;
    if (v == P4K_PAGE_EXECUTE_READWRITE) return P4K_PAGE_EXECUTE_WRITECOPY;
    return v;
}
/* readable / writable / executable as granted by a Win32 protection */
static int m_r( uint32_t v ) { return v != P4K_PAGE_NOACCESS; }   /* X-only is read by the emulator */
static int m_w( uint32_t v ) { return v == P4K_PAGE_READWRITE || v == P4K_PAGE_WRITECOPY ||
                                      v == P4K_PAGE_EXECUTE_READWRITE || v == P4K_PAGE_EXECUTE_WRITECOPY; }
static int m_x( uint32_t v ) { return v == P4K_PAGE_EXECUTE || v == P4K_PAGE_EXECUTE_READ ||
                                      v == P4K_PAGE_EXECUTE_READWRITE || v == P4K_PAGE_EXECUTE_WRITECOPY; }

static uintptr_t rd( uintptr_t v, uintptr_t m ) { return v & ~(m - 1); }
static uintptr_t ru( uintptr_t v, uintptr_t m ) { return (v + m - 1) & ~(m - 1); }
static unsigned pidx( uintptr_t a ) { return (unsigned)((a - WBASE) >> 12); }
static int m_find( uintptr_t a )
{
    if (a < WBASE || a >= WBASE + WSIZE) return -1;
    return M.pg[pidx( a )].alloc - 1;
}
static int m_range_free( uintptr_t b, size_t n )
{
    size_t i;
    for (i = 0; i < n >> 12; i++) if (M.pg[pidx( b ) + i].alloc) return 0;
    return 1;
}
static int m_in_window( uintptr_t b, size_t n ) { return b >= WBASE && b + n <= WBASE + WSIZE && b + n >= b; }

static int m_new_alloc( uintptr_t base, size_t size, uint32_t aprot, int image, int ww )
{
    int k;
    unsigned i;
    for (k = 0; k < MAXA; k++) if (!M.al[k].live) break;
    if (k == MAXA) fail( "model: too many allocations" );
    M.al[k] = (mall){ 1, base, size, aprot, image, ww };
    for (i = 0; i < size >> 12; i++)
        M.pg[pidx( base ) + i] = (mpage){ k + 1, 0, 0, 0, ww && !(ctx.flags & P4K_POLICY_SOFT_WRITEWATCH), 0, 0 };
    return k;
}

static int m_gap_exists( size_t size )
{
    uintptr_t b;
    for (b = WBASE; b + size <= WBASE + WSIZE; b += 0x10000)
        if (m_range_free( b, ru( size, 0x1000 ) )) return 1;
    return 0;
}

static int m_alloc( uintptr_t *base_p, size_t *size_p, uint32_t type, uint32_t protect, uintptr_t lib_base, int lib_ok )
{
    uintptr_t base = *base_p;
    size_t size = *size_p;
    uint32_t v = protect & 0xff;
    int k;
    unsigned i;

    if (!size) return P4K_E_INVALID_PARAMETER;
    if (type & ~(P4K_MEM_COMMIT | P4K_MEM_RESERVE | P4K_MEM_WRITE_WATCH)) return P4K_E_INVALID_PARAMETER;
    if (!(type & (P4K_MEM_COMMIT | P4K_MEM_RESERVE))) return P4K_E_INVALID_PARAMETER;
    if ((type & P4K_MEM_WRITE_WATCH) && !(type & P4K_MEM_RESERVE)) return P4K_E_INVALID_PARAMETER;
    if (base)
    {
        uintptr_t end = base + size;
        base = (type & P4K_MEM_RESERVE) ? rd( base, 0x10000 ) : rd( base, 0x1000 );
        size = ru( end, 0x1000 ) - base;
    }
    else size = ru( size, 0x1000 );

    if ((type & P4K_MEM_RESERVE) || !base)
    {
        if (!m_prot_valid( protect ) || m_is_wc( v )) return P4K_E_INVALID_PAGE_PROTECTION;
        if (base)
        {
            if (!m_in_window( base, size )) return P4K_E_INVALID_PARAMETER;
            if (!m_range_free( base, size )) return P4K_E_CONFLICT;
        }
        else
        {
            if (!lib_ok)
            {
                if (m_gap_exists( size )) fail( "library said NO_MEMORY but the model has a free gap for %zx", size );
                return P4K_E_NO_MEMORY;
            }
            base = lib_base;
            if ((base & 0xffff) || !m_in_window( base, size ) || !m_range_free( base, size ))
                fail( "library placed an allocation at invalid %lx+%zx", (unsigned long)base, size );
        }
        k = m_new_alloc( base, size, protect & (0xff | P4K_PAGE_GUARD), 0, (type & P4K_MEM_WRITE_WATCH) != 0 );
        for (i = 0; i < size >> 12; i++)
        {
            mpage *p = &M.pg[pidx( base ) + i];
            p->committed = (type & P4K_MEM_COMMIT) != 0;
            p->prot = v;
            p->guard = (protect & P4K_PAGE_GUARD) != 0;
        }
    }
    else
    {
        k = m_find( base );
        if (k < 0 || base + size > M.al[k].base + M.al[k].size) return P4K_E_NOT_MAPPED;
        if (M.al[k].image) return P4K_E_INVALID_PARAMETER;
        if (!m_prot_valid( protect ) || m_is_wc( v )) return P4K_E_INVALID_PAGE_PROTECTION;
        for (i = 0; i < size >> 12; i++)
        {
            mpage *p = &M.pg[pidx( base ) + i];
            p->committed = 1; p->prot = v; p->guard = (protect & P4K_PAGE_GUARD) != 0;
        }
    }
    *base_p = base; *size_p = size;
    return P4K_OK;
}

static int m_protect( uintptr_t *base_p, size_t *size_p, uint32_t protect, uint32_t *old )
{
    uintptr_t base = rd( *base_p, 0x1000 );
    size_t size = ru( *base_p + *size_p, 0x1000 ) - base;
    int k;
    unsigned i;
    uint32_t v = protect & 0xff;

    *old = P4K_PAGE_NOACCESS;
    if (!*size_p) return P4K_E_INVALID_PARAMETER;
    k = m_find( base );
    if (k < 0 || base + size > M.al[k].base + M.al[k].size) return P4K_E_INVALID_PARAMETER;
    for (i = 0; i < size >> 12; i++) if (!M.pg[pidx( base ) + i].committed) return P4K_E_NOT_COMMITTED;
    if (!m_prot_valid( protect )) return P4K_E_INVALID_PAGE_PROTECTION;
    if (!M.al[k].image && m_is_wc( v )) return P4K_E_INVALID_PAGE_PROTECTION;
    if (M.al[k].image) v = m_to_image( v );
    *old = M.pg[pidx( base )].prot | (M.pg[pidx( base )].guard ? P4K_PAGE_GUARD : 0);
    for (i = 0; i < size >> 12; i++)
    {
        mpage *p = &M.pg[pidx( base ) + i];
        p->prot = v; p->guard = (protect & P4K_PAGE_GUARD) != 0;
    }
    *base_p = base; *size_p = size;
    return P4K_OK;
}

static int m_decommit( uintptr_t base_in, size_t size_in )
{
    uintptr_t base = rd( base_in, 0x1000 );
    size_t size = size_in ? ru( base_in + size_in, 0x1000 ) - base : 0;
    int k;
    unsigned i;

    if (!base || (k = m_find( base )) < 0) return P4K_E_NOT_ALLOCATED;
    if (M.al[k].image) return P4K_E_INVALID_PARAMETER;
    if (!size && base != M.al[k].base) return P4K_E_NOT_AT_BASE;
    if (M.al[k].base + M.al[k].size - base < size) return P4K_E_UNABLE_TO_FREE;
    if (!size) size = M.al[k].size;
    for (i = 0; i < size >> 12; i++) { M.pg[pidx( base ) + i].committed = 0; M.pg[pidx( base ) + i].data = 0; }
    return P4K_OK;
}

static int m_release( uintptr_t base )
{
    int k = m_find( base );
    unsigned i;

    if (k < 0) return P4K_E_NOT_ALLOCATED;
    if (M.al[k].base != base) return P4K_E_NOT_AT_BASE;
    for (i = 0; i < M.al[k].size >> 12; i++) M.pg[pidx( base ) + i] = (mpage){ 0 };
    M.al[k].live = 0;
    return P4K_OK;
}

typedef struct { int ok; p4k_mbi r; } mq;
static mq m_query( uintptr_t addr )
{
    mq q;
    uintptr_t base = rd( addr, 0x1000 );
    int k = m_find( base );
    unsigned i, n;

    memset( &q, 0, sizeof(q) );
    q.ok = 1;
    q.r.base = base;
    if (k < 0)
    {
        uintptr_t e = base;
        while (e < WBASE + WSIZE && !M.pg[pidx( e )].alloc) e += 0x1000;
        q.r.region_size = e - base;
        q.r.state = P4K_MEM_FREE;
        q.r.protect = P4K_PAGE_NOACCESS;
        return q;
    }
    {
        mpage *p = &M.pg[pidx( base )];
        uintptr_t end = M.al[k].base + M.al[k].size;
        n = (unsigned)((end - base) >> 12);
        for (i = 1; i < n; i++)
        {
            mpage *o = &M.pg[pidx( base ) + i];
            if (o->committed != p->committed) break;
            if (p->committed && (o->prot != p->prot || o->guard != p->guard)) break;
        }
        q.r.region_size = (size_t)i << 12;
        q.r.alloc_base = M.al[k].base;
        q.r.alloc_protect = M.al[k].aprot;
        q.r.state = p->committed ? P4K_MEM_COMMIT : P4K_MEM_RESERVE;
        q.r.protect = p->committed ? p->prot | (p->guard ? P4K_PAGE_GUARD : 0) : 0;
        q.r.type = M.al[k].image ? P4K_MEM_IMAGE : P4K_MEM_PRIVATE;
    }
    return q;
}

/* ---------------------------------------------------------------- expected host state ---- */
static unsigned part_contrib( const mpage *p )
{
    unsigned r = 0;
    int soft_guard = (ctx.flags & P4K_POLICY_SOFT_GUARD) != 0;
    int soft_ww = (ctx.flags & P4K_POLICY_SOFT_WRITEWATCH) != 0;
    int dropx = (ctx.flags & P4K_POLICY_DROP_GUEST_X) != 0;

    if (!p->alloc || !p->committed) return 0;
    if (p->guard && !soft_guard) return 0;
    if (m_r( p->prot )) r |= P4K_HOST_READ;
    if (m_w( p->prot )) r |= P4K_HOST_WRITE | P4K_HOST_READ;
    if (m_x( p->prot ) && (!dropx || p->native)) r |= P4K_HOST_EXEC | P4K_HOST_READ;
    (void)soft_ww;
    return r;
}

/* guard / write watch armed on some committed part of the host page containing 4 KB page 'idx' */
static void host_armed( unsigned idx, int *guard, int *watch )
{
    unsigned parts = ctx.hps >> 12, first = idx / parts * parts, i;

    *guard = *watch = 0;
    for (i = 0; i < parts; i++)
    {
        const mpage *p = &M.pg[first + i];
        if (!p->alloc || !p->committed) continue;
        if (p->guard && !(ctx.flags & P4K_POLICY_SOFT_GUARD)) *guard = 1;
        if (p->clean && !(ctx.flags & P4K_POLICY_SOFT_WRITEWATCH)) *watch = 1;
    }
}

static int host_page_covered( unsigned hp_idx )
{
    uintptr_t hs = ctx.hps, a = WBASE + (uintptr_t)hp_idx * hs;
    int k;
    for (k = 0; k < MAXA; k++)
        if (M.al[k].live && M.al[k].base < a + hs && a < M.al[k].base + ru( M.al[k].size, hs )) return 1;
    return 0;
}

static unsigned expected_host_prot( unsigned hp_idx )
{
    unsigned parts = ctx.hps >> 12, i, prot = 0, first = hp_idx * parts;
    int g, w;

    for (i = 0; i < parts; i++) prot |= part_contrib( &M.pg[first + i] );
    host_armed( first, &g, &w );
    if (g) prot = 0;
    else if (w) prot &= ~(unsigned)P4K_HOST_WRITE;
    if ((ctx.flags & P4K_POLICY_ENFORCE_WX) && (prot & P4K_HOST_WRITE) && (prot & P4K_HOST_EXEC))
        prot &= ~(unsigned)P4K_HOST_EXEC;
    return prot;
}

/* ---------------------------------------------------------------- the checks ------------ */
static p4k_space *S;
static struct mock_host *H;

static void check_all( void )
{
    uintptr_t a;
    unsigned i, hpn = (unsigned)(WSIZE / ctx.hps);

    if (H->violations) fail( "host rule violated: %s", H->first_violation );

    /* VirtualQuery walk */
    for (a = WBASE; a < WBASE + WSIZE; )
    {
        p4k_mbi got;
        mq want = m_query( a );
        int st = p4k_space_query( S, a, &got );

        if (st != P4K_OK) fail( "query(%lx) status %d", (unsigned long)a, st );
        if (got.base != want.r.base || got.region_size != want.r.region_size || got.state != want.r.state ||
            got.protect != want.r.protect || got.alloc_base != want.r.alloc_base ||
            got.alloc_protect != want.r.alloc_protect || got.type != want.r.type)
            fail( "query(%lx): got base=%lx size=%lx state=%x prot=%x abase=%lx aprot=%x type=%x\n"
                  "                      want base=%lx size=%lx state=%x prot=%x abase=%lx aprot=%x type=%x",
                  (unsigned long)a,
                  (unsigned long)got.base, (unsigned long)got.region_size, got.state, got.protect,
                  (unsigned long)got.alloc_base, got.alloc_protect, got.type,
                  (unsigned long)want.r.base, (unsigned long)want.r.region_size, want.r.state, want.r.protect,
                  (unsigned long)want.r.alloc_base, want.r.alloc_protect, want.r.type );
        if (!want.r.region_size) fail( "zero-size region at %lx", (unsigned long)a );
        a += want.r.region_size;
    }
    /* mid-region addresses give the same region start */
    {
        uintptr_t x = WBASE + (uintptr_t)rn( NPG ) * 0x1000 + rn( 0x1000 );
        p4k_mbi got;
        mq want = m_query( x );
        if (p4k_space_query( S, x, &got ) || got.base != want.r.base || got.region_size != want.r.region_size)
            fail( "mid-page query(%lx) mismatch", (unsigned long)x );
    }

    for (i = 0; i < hpn; i++)
    {
        uintptr_t ha = WBASE + (uintptr_t)i * ctx.hps;
        int cov = host_page_covered( i );
        int hp = mock_prot_at( H, ha );

        if (cov != (hp >= 0)) fail( "host page %lx mapped=%d, model covered=%d", (unsigned long)ha, hp >= 0, cov );
        if (!cov) continue;
        {
            unsigned parts = ctx.hps >> 12, j, any_w = 0, any_x = 0, differs = 0, c0 = part_contrib( &M.pg[i * parts] );
            for (j = 0; j < parts; j++)
            {
                unsigned c = part_contrib( &M.pg[i * parts + j] );
                if (c != c0) differs = 1;
                if (c & P4K_HOST_WRITE) any_w = 1;
                if (c & P4K_HOST_EXEC) any_x = 1;
                if (M.pg[i * parts + j].guard) stats.guard_pages++;
                if (M.pg[i * parts + j].native) stats.native_pages++;
            }
            stats.mixed_host_pages += differs;
            stats.wx_union_pages += any_w && any_x;
        }
        if ((unsigned)hp != expected_host_prot( i ))
            fail( "host page %lx prot %d, expected union %u", (unsigned long)ha, hp, expected_host_prot( i ) );
    }

    /* no legitimate access may fault, contents preserved / zero-filled */
    for (i = 0; i < NPG; i++)
    {
        mpage *p = &M.pg[i];
        uintptr_t a4 = WBASE + (uintptr_t)i * 0x1000;
        int hp;

        if (!p->alloc) continue;
        hp = mock_prot_at( H, a4 );
        if (p->committed)
        {
            int g, w;

            host_armed( i, &g, &w );   /* a deliberately armed guard / watch may fault: that is the feature */
            if (!g)
            {
                if (m_r( p->prot ) && !(hp & P4K_HOST_READ)) fail( "page %lx readable in Windows, host prot %d", (unsigned long)a4, hp );
                if (m_w( p->prot ) && !w && !(hp & P4K_HOST_WRITE))
                    fail( "page %lx writable in Windows, host prot %d", (unsigned long)a4, hp );
            }
            if (H->mem[a4 - WBASE] != p->data || H->mem[a4 - WBASE + 4095] != p->data)
                fail( "page %lx content %02x/%02x, model %02x", (unsigned long)a4, H->mem[a4 - WBASE],
                      H->mem[a4 - WBASE + 4095], p->data );
        }
        else
        {
            if (H->mem[a4 - WBASE] || H->mem[a4 - WBASE + 4095] || H->mem[a4 - WBASE + 2048])
                fail( "decommitted/reserved page %lx is not zero (stale data)", (unsigned long)a4 );
        }
    }
}

/* ---------------------------------------------------------------- operations ------------ */
static const uint32_t prots[] = { P4K_PAGE_NOACCESS, P4K_PAGE_READONLY, P4K_PAGE_READWRITE, P4K_PAGE_EXECUTE,
                                  P4K_PAGE_EXECUTE_READ, P4K_PAGE_EXECUTE_READWRITE };

static uint32_t rand_prot( int allow_bad )
{
    uint32_t p = prots[rn( 6 )];
    unsigned r = rn( 100 );

    if (r < 12 && p != P4K_PAGE_NOACCESS) p |= P4K_PAGE_GUARD;
    if (allow_bad)
    {
        if (r >= 96) p = P4K_PAGE_WRITECOPY;            /* invalid for private memory */
        else if (r >= 93) p = 0x3;                      /* two bits */
        else if (r >= 91) p = P4K_PAGE_EXECUTE_WRITECOPY;
    }
    return p;
}

static int pick_alloc( void )
{
    int k, tries;
    for (tries = 0; tries < 20; tries++) { k = rn( MAXA ); if (M.al[k].live) return k; }
    for (k = 0; k < MAXA; k++) if (M.al[k].live) return k;
    return -1;
}

/* random sub-range of (or around) an allocation, with unaligned edges */
static void pick_range( int k, uintptr_t *b, size_t *s )
{
    mall *a = &M.al[k];
    size_t pages = a->size >> 12;
    size_t off = rn( (unsigned)pages ), len = 1 + rn( (unsigned)(pages - off + (rn( 8 ) == 0 ? 2 : 0)) );

    *b = a->base + off * 0x1000 + (rn( 4 ) == 0 ? rn( 0x1000 ) : 0);
    *s = len * 0x1000 - (rn( 4 ) == 0 ? rn( 0x800 ) : 0);
    if (!*s) *s = 1;
    if (rn( 10 ) == 0) *s = a->size - (*b - a->base);   /* up to the end */
}

static void op_alloc( void )
{
    uintptr_t base = 0, tbase;
    size_t size = (1 + (rn( 3 ) ? rn( 20 ) : rn( 90 ))) * 0x1000 - (rn( 6 ) == 0 ? rn( 0x800 ) : 0), tsize;
    unsigned r = rn( 100 );
    uint32_t type = r < 30 ? P4K_MEM_RESERVE : r < 80 ? (P4K_MEM_RESERVE | P4K_MEM_COMMIT) : r < 85 ? P4K_MEM_COMMIT
                  : r < 90 ? (P4K_MEM_RESERVE | P4K_MEM_WRITE_WATCH) : r < 97 ? (P4K_MEM_RESERVE | P4K_MEM_COMMIT | P4K_MEM_WRITE_WATCH) : 0;
    uint32_t protect = rand_prot( 1 );
    int st, mst;

    if (rn( 3 ) == 0) base = WBASE + (uintptr_t)rn( NPG / 16 ) * 0x10000 + (rn( 5 ) == 0 ? rn( 0x10000 ) : 0);
    snprintf( ctx.desc, sizeof(ctx.desc), "alloc base=%lx size=%zx type=%x prot=%x", (unsigned long)base, size, type, protect );
    tbase = base; tsize = size;
    st = p4k_space_alloc( S, &base, &size, type, protect );
    mst = m_alloc( &tbase, &tsize, type, protect, base, st == P4K_OK );
    if (st != mst) fail( "alloc status %d, model %d", st, mst );
    if (st == P4K_OK) stats.alloc_ok++; else stats.errors++;
    if (st == P4K_OK && (base != tbase || size != tsize))
        fail( "alloc result %lx+%zx, model %lx+%zx", (unsigned long)base, size, (unsigned long)tbase, tsize );
}

static void op_protect( void )
{
    int k = pick_alloc();
    uintptr_t b, tb;
    size_t s, ts;
    uint32_t prot = rand_prot( 1 ), old = 0xdead, mold = 0xdead;
    int st, mst;

    if (k < 0) return;
    pick_range( k, &b, &s );
    tb = b; ts = s;
    snprintf( ctx.desc, sizeof(ctx.desc), "protect %lx+%zx prot=%x", (unsigned long)b, s, prot );
    st = p4k_space_protect( S, &b, &s, prot, &old );
    mst = m_protect( &tb, &ts, prot, &mold );
    if (st != mst) fail( "protect status %d, model %d", st, mst );
    if (old != mold) fail( "protect old %x, model %x", old, mold );
    if (st == P4K_OK) stats.protect_ok++; else stats.errors++;
    if (st == P4K_OK && (b != tb || s != ts)) fail( "protect range %lx+%zx vs %lx+%zx", (unsigned long)b, s, (unsigned long)tb, ts );
}

static void op_commit( void )
{
    int k = pick_alloc();
    uintptr_t b, tb;
    size_t s, ts;
    uint32_t prot = rand_prot( 1 );
    int st, mst;

    if (k < 0) return;
    pick_range( k, &b, &s );
    tb = b; ts = s;
    snprintf( ctx.desc, sizeof(ctx.desc), "commit %lx+%zx prot=%x", (unsigned long)b, s, prot );
    st = p4k_space_alloc( S, &b, &s, P4K_MEM_COMMIT, prot );
    mst = m_alloc( &tb, &ts, P4K_MEM_COMMIT, prot, 0, 1 );
    if (st != mst) fail( "commit status %d, model %d", st, mst );
    if (st == P4K_OK) stats.commit_ok++; else stats.errors++;
}

static void op_decommit( void )
{
    int k = pick_alloc();
    uintptr_t b;
    size_t s;
    int st, mst;

    if (k < 0) return;
    pick_range( k, &b, &s );
    if (rn( 8 ) == 0) { b = M.al[k].base; s = 0; }
    if (rn( 25 ) == 0) b += 0x20000000;
    snprintf( ctx.desc, sizeof(ctx.desc), "decommit %lx+%zx", (unsigned long)b, s );
    st = p4k_space_decommit( S, b, s );
    mst = m_decommit( b, s );
    if (st != mst) fail( "decommit status %d, model %d", st, mst );
    if (st == P4K_OK)
    {
        uintptr_t be = rd( b, 0x1000 ), en = s ? ru( b + s, 0x1000 ) : be;
        stats.decommit_ok++;
        if ((be & (ctx.hps - 1)) || (en & (ctx.hps - 1))) stats.partial_decommit++;
    }
    else stats.errors++;
}

static void op_release( void )
{
    int k = pick_alloc();
    uintptr_t b;
    int st, mst;

    if (k < 0) return;
    b = M.al[k].base + (rn( 6 ) == 0 ? 0x1000 * (1 + rn( 3 )) : 0);
    snprintf( ctx.desc, sizeof(ctx.desc), "release %lx", (unsigned long)b );
    st = p4k_space_release( S, b );
    mst = m_release( b );
    if (st != mst) fail( "release status %d, model %d", st, mst );
    if (st == P4K_OK) stats.release_ok++; else stats.errors++;
}

static void op_write( void )
{
    unsigned tries, i;
    for (tries = 0; tries < 30; tries++)
    {
        mpage *p;
        uintptr_t a;
        uint8_t v = (uint8_t)(1 + rn( 250 ));

        i = rn( NPG );
        p = &M.pg[i];
        int g, w;

        if (!p->alloc || !p->committed || !m_w( p->prot )) continue;
        host_armed( i, &g, &w );
        if (g || w) continue;
        a = WBASE + (uintptr_t)i * 0x1000;
        snprintf( ctx.desc, sizeof(ctx.desc), "write page %lx = %02x", (unsigned long)a, v );
        if (!mock_write8( H, a, v ) || !mock_write8( H, a + 4095, v )) fail( "host refused a legitimate write" );
        p->data = v;
        stats.write_ok++;
        return;
    }
}

static void op_wwget( void )
{
    int k = pick_alloc();
    uintptr_t b, addrs[2048], wl[2048], rb;
    size_t s, count, cap, i, want = 0, expect;
    int st, reset = rn( 2 );

    if (k < 0) return;
    pick_range( k, &b, &s );
    cap = count = 1 + rn( 300 );
    snprintf( ctx.desc, sizeof(ctx.desc), "get_write_watch %lx+%zx reset=%d cap=%zu", (unsigned long)b, s, reset, cap );
    st = p4k_space_get_write_watch( S, reset, b, s, addrs, &count );
    {
        uintptr_t tb = rd( b, 0x1000 );
        size_t ts = ru( b + s, 0x1000 ) - tb;
        int mk = m_find( tb );
        int mok = mk >= 0 && tb + ts <= M.al[mk].base + M.al[mk].size && M.al[mk].ww;

        if ((st == P4K_OK) != mok) fail( "write watch status %d, model ok=%d", st, mok );
        if (st != P4K_OK) return;
        /* pages written = pages whose "clean" flag is clear; soft mode reports every page.
         * The returned list is the first min(capacity, written) of them (ascending). */
        for (rb = tb; rb < tb + ts && want < 2048; rb += 0x1000)
            if (!M.pg[pidx( rb )].clean) wl[want++] = rb;
        expect = want < cap ? want : cap;
        if (count != expect) fail( "write watch returned %zu pages, model %zu", count, expect );
        for (i = 0; i < count; i++)
            if (addrs[i] != wl[i]) fail( "write watch entry %zu: %lx vs %lx", i, (unsigned long)addrs[i], (unsigned long)wl[i] );
        stats.ww_ok++;
        if (reset && !(ctx.flags & P4K_POLICY_SOFT_WRITEWATCH))
            for (rb = tb; rb < tb + ts; rb += 0x1000) M.pg[pidx( rb )].clean = 1;
    }
}

static void op_note_write( void )
{
    int k = pick_alloc();
    uintptr_t a, hs = ctx.hps, hp;
    int st;

    if (k < 0 || (ctx.flags & P4K_POLICY_SOFT_WRITEWATCH)) return;
    a = M.al[k].base + (uintptr_t)rn( (unsigned)(M.al[k].size >> 12) ) * 0x1000;
    snprintf( ctx.desc, sizeof(ctx.desc), "note_write %lx", (unsigned long)a );
    st = p4k_space_note_write( S, a );
    if (!M.al[k].ww) { if (st == P4K_OK) fail( "note_write on non-watched alloc succeeded" ); return; }
    if (st != P4K_OK) fail( "note_write status %d", st );
    hp = rd( a, hs );
    for (a = hp; a < hp + hs; a += 0x1000)
        if (m_find( a ) >= 0) M.pg[pidx( a )].clean = 0;
}

static void op_image( void )
{
    p4k_image_section sec[8];
    unsigned n = 1 + rn( 6 ), i, hdr_pages = 1 + rn( 2 ), total;
    uint32_t hdr_size = hdr_pages * 0x1000 - rn( 0xc00 );
    uint32_t rva;
    uintptr_t base = rn( 3 ) == 0 ? WBASE + (uintptr_t)rn( NPG / 16 ) * 0x10000 : 0, tbase;
    static const uint32_t chars[] = { 0x40000040, 0x60000020, 0xC0000040, 0x20000000, 0x80000000, 0xE0000020,
                                      0x40000000, 0x00000000, 0xA0000000 };
    int st, mst, k;
    unsigned cnt;

    rva = hdr_pages * 0x1000;
    for (i = 0; i < n; i++)
    {
        sec[i].rva = rva;
        sec[i].size = (1 + rn( 8 )) * 0x1000;
        sec[i].characteristics = chars[rn( 9 )];
        sec[i].native = rn( 7 ) == 0;
        rva += sec[i].size;
    }
    total = rva;
    snprintf( ctx.desc, sizeof(ctx.desc), "image base=%lx size=%x hdr=%x nsec=%u", (unsigned long)base, total, hdr_size, n );
    tbase = base;
    st = p4k_space_image_begin( S, &base, total );

    /* model begin */
    if (tbase)
    {
        if ((tbase & 0xffff) || !m_in_window( tbase, total )) mst = P4K_E_INVALID_PARAMETER;
        else if (!m_range_free( tbase, total )) mst = P4K_E_CONFLICT;
        else mst = P4K_OK;
    }
    else if (st == P4K_OK) mst = P4K_OK;
    else { if (m_gap_exists( total )) fail( "image: NO_MEMORY but model has a gap" ); mst = P4K_E_NO_MEMORY; }
    if (st != mst) fail( "image_begin status %d, model %d", st, mst );
    if (st != P4K_OK) return;
    if (tbase && base != tbase) fail( "image_begin moved a fixed base" );
    if ((base & 0xffff) || !m_in_window( base, total ) || !m_range_free( base, total ))
        fail( "image placed at invalid %lx", (unsigned long)base );
    k = m_new_alloc( base, total, P4K_PAGE_EXECUTE_WRITECOPY, 1, 0 );
    for (cnt = 0; cnt < total >> 12; cnt++)
    {
        mpage *p = &M.pg[pidx( base ) + cnt];
        p->committed = 1; p->prot = P4K_PAGE_WRITECOPY; p->guard = 0;
    }
    check_all();    /* pass-1 state must already be consistent */

    snprintf( ctx.desc, sizeof(ctx.desc), "image_finalize base=%lx", (unsigned long)base );
    st = p4k_space_image_finalize( S, base, hdr_size, sec, n );
    if (st != P4K_OK) fail( "image_finalize status %d", st );
    for (cnt = 0; cnt < hdr_pages; cnt++) M.pg[pidx( base ) + cnt].prot = P4K_PAGE_READONLY;
    for (i = 0; i < n; i++)
    {
        int r = (sec[i].characteristics & 0x40000000) != 0, w = (sec[i].characteristics & 0x80000000) != 0,
            x = (sec[i].characteristics & 0x20000000) != 0;
        uint32_t prot = w ? (x ? P4K_PAGE_EXECUTE_WRITECOPY : P4K_PAGE_WRITECOPY)
                          : x ? (r ? P4K_PAGE_EXECUTE_READ : P4K_PAGE_EXECUTE)
                              : (r ? P4K_PAGE_READONLY : P4K_PAGE_NOACCESS);
        for (cnt = 0; cnt < sec[i].size >> 12; cnt++)
        {
            mpage *p = &M.pg[pidx( base + sec[i].rva ) + cnt];
            p->prot = prot;
            p->native = sec[i].native;
        }
    }
    (void)k;
    stats.image_ok++;
}

static void op_query( void )
{
    uintptr_t a = WBASE + (uintptr_t)rn( NPG ) * 0x1000 + rn( 0x1000 );
    p4k_mbi got;
    mq want = m_query( a );
    int st;

    snprintf( ctx.desc, sizeof(ctx.desc), "query %lx", (unsigned long)a );
    st = p4k_space_query( S, a, &got );
    if (st != P4K_OK || got.base != want.r.base || got.region_size != want.r.region_size || got.state != want.r.state ||
        got.protect != want.r.protect) fail( "query mismatch" );
    /* outside of the window */
    if (p4k_space_query( S, WBASE + WSIZE, &got ) != P4K_E_INVALID_PARAMETER) fail( "query past the window succeeded" );
}

static void run_sequence( void )
{
    unsigned nops = 40 + rn( 80 );

    for (ctx.op = 0; ctx.op < nops; ctx.op++)
    {
        unsigned r = rn( 100 );
        ctx.desc[0] = 0;
        if (r < 20) op_alloc();
        else if (r < 34) op_commit();
        else if (r < 52) op_protect();
        else if (r < 62) op_decommit();
        else if (r < 68) op_release();
        else if (r < 72) op_query();
        else if (r < 84) op_write();
        else if (r < 90) op_image();
        else if (r < 95) op_wwget();
        else op_note_write();
        check_all();
    }
}

int main( int argc, char **argv )
{
    static const struct { const char *name; unsigned flags; } configs[] = {
        { "ios (drop-X, W^X, soft guard, soft write-watch)", P4K_POLICY_DROP_GUEST_X | P4K_POLICY_ENFORCE_WX | P4K_POLICY_SOFT_GUARD | P4K_POLICY_SOFT_WRITEWATCH },
        { "macos-ci (drop-X, W^X, fault guard+write-watch)", P4K_POLICY_DROP_GUEST_X | P4K_POLICY_ENFORCE_WX },
        { "linux (no policy; stock Wine behaviour)", 0 },
        { "wx-only (X passes through, W|X resolved)", P4K_POLICY_ENFORCE_WX | P4K_POLICY_SOFT_GUARD | P4K_POLICY_SOFT_WRITEWATCH },
    };
    static const unsigned hpss[] = { 4096, 16384, 65536 };
    unsigned nseq = argc > 1 ? (unsigned)atoi( argv[1] ) : 400;
    unsigned c, h, q;
    unsigned long total_seq = 0, total_ops = 0;

    for (c = 0; c < sizeof(configs) / sizeof(configs[0]); c++)
        for (h = 0; h < sizeof(hpss) / sizeof(hpss[0]); h++)
        {
            p4k_policy pol = { hpss[h], configs[c].flags };
            ctx.name = configs[c].name; ctx.hps = hpss[h]; ctx.flags = configs[c].flags;
            ctx.seed = 0x9e3779b97f4a7c15ul ^ (c * 1000003ul + h * 7919ul + 1);
            rng_state = ctx.seed;
            for (q = 0; q < nseq; q++)
            {
                p4k_space_host sh;

                memset( &M, 0, sizeof(M) );
                H = mock_new( WBASE, WSIZE, hpss[h], (configs[c].flags & P4K_POLICY_ENFORCE_WX) != 0 );
                sh = mock_space_host( H );
                S = p4k_space_new( &pol, &sh, WBASE, WSIZE );
                if (!S) { printf( "p4k_space_new failed\n" ); return 1; }
                ctx.seq = q;
                run_sequence();
                total_ops += ctx.op;
                total_seq++;
                p4k_space_free( S );
                mock_free( H );
            }
            printf( "  ok: %-52s host page %5u: %u sequences\n", configs[c].name, hpss[h], nseq );
        }
    printf( "coverage: allocs %lu, commits %lu, protects %lu, decommits %lu (%lu leaving a host page partly committed), "
            "releases %lu, images %lu, host-visible writes %lu, write-watch queries %lu, rejected calls %lu\n"
            "          host-page checks: %lu pages mixing different 4 KB protections, %lu pages whose union had W and X, "
            "%lu guard parts, %lu native parts\n",
            stats.alloc_ok, stats.commit_ok, stats.protect_ok, stats.decommit_ok, stats.partial_decommit,
            stats.release_ok, stats.image_ok, stats.write_ok, stats.ww_ok, stats.errors,
            stats.mixed_host_pages, stats.wx_union_pages, stats.guard_pages, stats.native_pages );
    printf( "test_random: all passed (%lu sequences, %lu operations, each followed by a full-state check)\n",
            total_seq, total_ops );
    return 0;
}
