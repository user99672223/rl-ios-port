/* Hand-written unit tests for prot4k (core + space) against the mock 16 KB host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_host.h"
#include "check.h"

int g_failures;

#define BASE 0x10000000u
#define WINSZ (4u << 20)

#define F_IOS (P4K_POLICY_DROP_GUEST_X | P4K_POLICY_ENFORCE_WX | P4K_POLICY_SOFT_GUARD | P4K_POLICY_SOFT_WRITEWATCH)

static p4k_space *mk( struct mock_host **mh, uintptr_t hps, unsigned flags )
{
    p4k_policy pol = { hps, flags };
    p4k_space_host h;

    *mh = mock_new( BASE, WINSZ, hps, (flags & P4K_POLICY_ENFORCE_WX) != 0 );
    h = mock_space_host( *mh );
    return p4k_space_new( &pol, &h, BASE, WINSZ );
}

static void done( p4k_space *s, struct mock_host *m )
{
    CHECK_EQ( m->violations, 0u );
    if (m->violations) printf( "    first violation: %s\n", m->first_violation );
    p4k_space_free( s );
    mock_free( m );
}

/* ---------------------------------------------------------------------------------------- */
static void test_policy_and_conversion( void )
{
    p4k_policy p = { 16384, 0 };
    uint8_t v;
    uint32_t prot;

    printf( "policy_and_conversion\n" );
    CHECK_EQ( p4k_policy_check( &p ), 0 );
    p.host_page_size = 4096;  CHECK_EQ( p4k_policy_check( &p ), 0 );
    p.host_page_size = 65536; CHECK_EQ( p4k_policy_check( &p ), 0 );
    p.host_page_size = 2048;  CHECK( p4k_policy_check( &p ) != 0 );
    p.host_page_size = 12288; CHECK( p4k_policy_check( &p ) != 0 );
    p.host_page_size = 131072; CHECK( p4k_policy_check( &p ) != 0 );

    /* every valid protection round-trips for private memory */
    {
        static const uint32_t valid[] = { P4K_PAGE_NOACCESS, P4K_PAGE_READONLY, P4K_PAGE_READWRITE,
                                          P4K_PAGE_EXECUTE, P4K_PAGE_EXECUTE_READ,
                                          P4K_PAGE_EXECUTE_READWRITE };
        unsigned i;
        for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        {
            CHECK_EQ( p4k_vprot_from_win32( valid[i], 0, &v ), 0 );
            CHECK_EQ( p4k_win32_from_vprot( v | P4K_COMMITTED ), valid[i] );
            CHECK_EQ( p4k_vprot_from_win32( valid[i] | P4K_PAGE_GUARD, 0, &v ), 0 );
            CHECK_EQ( p4k_win32_from_vprot( v ), valid[i] | P4K_PAGE_GUARD );
        }
    }
    /* image rule */
    CHECK_EQ( p4k_vprot_from_win32( P4K_PAGE_READWRITE, 1, &v ), 0 );
    CHECK_EQ( p4k_win32_from_vprot( v ), P4K_PAGE_WRITECOPY );
    CHECK_EQ( p4k_vprot_from_win32( P4K_PAGE_EXECUTE_READWRITE, 1, &v ), 0 );
    CHECK_EQ( p4k_win32_from_vprot( v ), P4K_PAGE_EXECUTE_WRITECOPY );
    /* invalid */
    for (prot = 0; prot < 0x200; prot++)
    {
        int valid_value = (prot & 0xff) == 1 || (prot & 0xff) == 2 || (prot & 0xff) == 4 || (prot & 0xff) == 8 ||
                          (prot & 0xff) == 0x10 || (prot & 0xff) == 0x20 || (prot & 0xff) == 0x40 || (prot & 0xff) == 0x80;
        CHECK_EQ( p4k_vprot_from_win32( prot, 0, &v ) == 0, valid_value );
    }
}

/* ---------------------------------------------------------------------------------------- */
static void test_union( void )
{
    p4k_policy plain = { 16384, 0 };
    p4k_policy ios = { 16384, F_IOS };
    p4k_policy fault = { 16384, P4K_POLICY_DROP_GUEST_X | P4K_POLICY_ENFORCE_WX };
    const uint8_t C = P4K_COMMITTED;
    uint8_t p[4];
    int conflict;

    printf( "union\n" );
    /* no policy: plain most-permissive union, as upstream Wine's get_host_page_vprot() + get_unix_prot() */
    p[0] = C | P4K_READ | P4K_EXEC; p[1] = C | P4K_READ | P4K_WRITE; p[2] = 0; p[3] = 0;
    CHECK_EQ( p4k_host_prot( &plain, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE | P4K_HOST_EXEC );
    CHECK_EQ( conflict, 0 );

    /* ios: guest X is dropped, no W|X, readable for the emulator */
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK_EQ( conflict, 0 );
    /* guest-only RX page alone: R, never X */
    p[1] = 0;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ );
    /* native RX alone keeps X */
    p[0] |= P4K_NATIVE;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_EXEC );
    /* native RX next to a writable part: conflict -> X dropped, reported */
    p[1] = C | P4K_READ | P4K_WRITE;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK_EQ( conflict, 1 );
    /* execute-only (PAGE_EXECUTE) guest page is readable on the host */
    p[0] = C | P4K_EXEC; p[1] = 0;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ );
    /* write-copy counts as writable */
    p[0] = C | P4K_READ | P4K_WRITECOPY;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    /* decommitted parts contribute nothing, even with R/W bits remembered */
    p[0] = P4K_READ | P4K_WRITE; p[1] = 0;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), 0 );
    /* set/clear are applied per part */
    CHECK_EQ( p4k_host_prot( &ios, p, 4, C, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    p[0] = C | P4K_READ | P4K_WRITE;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, P4K_WRITE, &conflict ), P4K_HOST_READ );

    /* guard: soft keeps access ... */
    p[0] = C | P4K_READ | P4K_WRITE | P4K_GUARD; p[1] = p[2] = p[3] = 0;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    /* ... fault mode: a fully guarded host page is NONE, one unguarded sibling keeps it RW */
    CHECK_EQ( p4k_host_prot( &fault, p, 4, 0, 0, &conflict ), 0 );
    /* ... and a guard arms the whole host page even with an unguarded sibling (stock Wine behaviour:
     * the guard fires on first access, over-reporting for the neighbour) */
    p[1] = C | P4K_READ | P4K_WRITE;
    CHECK_EQ( p4k_host_prot( &fault, p, 4, 0, 0, &conflict ), 0 );
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );

    /* write watch: soft never removes W; fault mode makes the whole host page read-only */
    p[0] = C | P4K_READ | P4K_WRITE | P4K_WRITEWATCH; p[1] = 0;
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK_EQ( p4k_host_prot( &fault, p, 4, 0, 0, &conflict ), P4K_HOST_READ );
    p[1] = C | P4K_READ | P4K_WRITE;
    CHECK_EQ( p4k_host_prot( &fault, p, 4, 0, 0, &conflict ), P4K_HOST_READ );
    CHECK_EQ( p4k_host_prot( &ios, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );
    /* a watch bit on a decommitted part arms nothing */
    p[0] = C | P4K_READ | P4K_WRITE; p[1] = P4K_WRITEWATCH;
    CHECK_EQ( p4k_host_prot( &fault, p, 4, 0, 0, &conflict ), P4K_HOST_READ | P4K_HOST_WRITE );

    /* 4 KB host page: parts == 1, identical to the per-page rule */
    {
        p4k_policy p4 = { 4096, F_IOS };
        uint8_t one = C | P4K_READ | P4K_EXEC;
        CHECK_EQ( p4k_host_prot( &p4, &one, 1, 0, 0, &conflict ), P4K_HOST_READ );
    }
}

/* ---------------------------------------------------------------------------------------- */
/* The union with an empty policy must equal what stock Wine computes today. The two helpers below are
 * transcribed from external/wine dlls/ntdll/unix/virtual.c (get_host_page_vprot, lines 1126-1142 and
 * get_unix_prot, lines 1381-1394 at de56343b), with PROT_* replaced by the P4K_HOST_* values. */
static unsigned wine_get_unix_prot( uint8_t vprot )
{
    unsigned prot = 0;
    if ((vprot & P4K_COMMITTED) && !(vprot & P4K_GUARD))
    {
        if (vprot & P4K_READ) prot |= P4K_HOST_READ;
        if (vprot & P4K_WRITE) prot |= P4K_HOST_WRITE | P4K_HOST_READ;
        if (vprot & P4K_WRITECOPY) prot |= P4K_HOST_WRITE | P4K_HOST_READ;
        if (vprot & P4K_EXEC) prot |= P4K_HOST_EXEC | P4K_HOST_READ;
        if (vprot & P4K_WRITEWATCH) prot &= ~(unsigned)P4K_HOST_WRITE;
    }
    return prot;
}
static unsigned wine_host_prot( const uint8_t *parts, unsigned n, uint8_t set, uint8_t clear )
{
    uint8_t vprot = 0;
    unsigned i;
    for (i = 0; i < n; i++) vprot |= parts[i];       /* get_host_page_vprot */
    return wine_get_unix_prot( (uint8_t)((vprot & ~clear) | set) );   /* mprotect_range: (vprot & ~clear) | set */
}

static void test_equals_stock_wine( void )
{
    static const unsigned sizes[] = { 4096, 16384, 65536 };
    unsigned long long x = 88172645463325252ull;
    unsigned si, iter;
    unsigned long checked = 0;

    printf( "equals_stock_wine (empty policy vs transcribed Wine logic)\n" );
    for (si = 0; si < 3; si++)
    {
        p4k_policy pol = { sizes[si], 0 };
        unsigned n = sizes[si] >> 12;
        for (iter = 0; iter < 200000; iter++)
        {
            uint8_t parts[P4K_MAX_PARTS], clear = 0;
            unsigned i;
            int conflict;

            for (i = 0; i < n; i++)
            {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                parts[i] = (uint8_t)(x >> 20);
                /* states reachable through the Win32 API: a decommitted part carries no other bit,
                 * and the NATIVE bit does not exist in stock Wine */
                if (!(parts[i] & P4K_COMMITTED)) parts[i] = 0;
                parts[i] &= (uint8_t)~P4K_NATIVE;
            }
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            if ((x & 7) == 0) clear = P4K_WRITEWATCH;   /* check_write_access(): temporarily enable writes */
            checked++;
            CHECK_EQ( p4k_host_prot( &pol, parts, n, 0, clear, &conflict ), wine_host_prot( parts, n, 0, clear ) );
            /* mprotect_range( set = COMMITTED ) is equal when every part is committed already */
            for (i = 0; i < n; i++) parts[i] |= P4K_COMMITTED;
            CHECK_EQ( p4k_host_prot( &pol, parts, n, P4K_COMMITTED, 0, &conflict ), wine_host_prot( parts, n, P4K_COMMITTED, 0 ) );
            if (g_failures > 5) return;
        }
    }
    printf( "  %lu part combinations compared\n", checked * 2 );
}

/* ---------------------------------------------------------------------------------------- */
struct tbl { uint8_t v[64]; };
static uint8_t tbl_get( void *ud, uintptr_t page ) { struct tbl *t = ud; return page < 64 ? t->v[page] : 0; }

struct rec { int n; uintptr_t a[16]; size_t l[16]; int p[16]; };
static int rec_mprotect( void *ud, uintptr_t addr, size_t len, int prot )
{
    struct rec *r = ud;
    if (r->n < 16) { r->a[r->n] = addr; r->l[r->n] = len; r->p[r->n] = prot; }
    r->n++;
    return 0;
}
static int rec_zero( void *ud, uintptr_t a, size_t l ) { (void)ud; (void)a; (void)l; return 0; }

static void test_apply_range_coalescing( void )
{
    p4k_policy pol = { 16384, F_IOS };
    struct tbl t;
    struct rec r = { 0 };
    p4k_host_ops host = { rec_mprotect, rec_zero, &r, NULL };
    const uint8_t RW = P4K_COMMITTED | P4K_READ | P4K_WRITE, RO = P4K_COMMITTED | P4K_READ;
    int i;

    printf( "apply_range_coalescing\n" );
    memset( &t, 0, sizeof(t) );
    /* host pages (16 KB = 4 parts): 0:RW 1:RW 2:RO 3:RO 4:none 5:RW  (page index in 4 KB units: *4) */
    for (i = 0; i < 8; i++) t.v[i] = RW;
    for (i = 8; i < 16; i++) t.v[i] = RO;
    t.v[20] = RW;
    CHECK_EQ( p4k_apply_range( &pol, tbl_get, &t, &host, 0, 6 * 16384, 0, 0 ), 0 );
    /* host pages: 0,1 RW; 2,3 RO; 4 = part 16 none, 5 = part 20 RW => runs: [0,2) RW, [2,4) RO, [4,5) NONE, [5,6) RW */
    CHECK_EQ( r.n, 4 );
    CHECK_EQ( r.a[0], 0u );        CHECK_EQ( r.l[0], 2u * 16384 ); CHECK_EQ( r.p[0], 3 );
    CHECK_EQ( r.a[1], 2u * 16384 ); CHECK_EQ( r.l[1], 2u * 16384 ); CHECK_EQ( r.p[1], 1 );
    CHECK_EQ( r.a[2], 4u * 16384 ); CHECK_EQ( r.l[2], 16384u );     CHECK_EQ( r.p[2], 0 );
    CHECK_EQ( r.a[3], 5u * 16384 ); CHECK_EQ( r.l[3], 16384u );     CHECK_EQ( r.p[3], 3 );

    /* an unaligned 4 KB range still produces one host-page-aligned call */
    memset( &r, 0, sizeof(r) );
    CHECK_EQ( p4k_apply_range( &pol, tbl_get, &t, &host, 3 * 4096, 4096, 0, 0 ), 0 );
    CHECK_EQ( r.n, 1 ); CHECK_EQ( r.a[0], 0u ); CHECK_EQ( r.l[0], 16384u );

    /* run_length: stops at the first differing page, honours the mask */
    {
        uint8_t first;
        t.v[2] = RW | P4K_WRITEWATCH;
        CHECK_EQ( p4k_run_length( tbl_get, &t, 0, 8 * 4096, (uint8_t)~P4K_PRIVATE_BITS, &first ), 8u * 4096 );
        CHECK_EQ( p4k_run_length( tbl_get, &t, 0, 8 * 4096, 0xff, &first ), 2u * 4096 );
        CHECK_EQ( first, RW );
    }
}

/* ---------------------------------------------------------------------------------------- */
static void test_space_basic_4k_semantics( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0;
    size_t size = 0x10000;
    p4k_mbi mbi;
    uintptr_t b2;
    size_t s2;
    uint32_t old;

    printf( "space_basic_4k_semantics (host page 16 KB)\n" );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE, P4K_PAGE_READWRITE ), P4K_OK );
    CHECK_EQ( size, 0x10000u );
    CHECK_EQ( base & 0xffff, 0u );

    /* nothing committed yet: host NONE, query reserve */
    CHECK_EQ( mock_prot_at( m, base ), 0 );
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_RESERVE ); CHECK_EQ( mbi.region_size, 0x10000u );
    CHECK_EQ( mbi.alloc_base, base );  CHECK_EQ( mbi.alloc_protect, P4K_PAGE_READWRITE );
    CHECK_EQ( mbi.type, P4K_MEM_PRIVATE ); CHECK_EQ( mbi.protect, 0u );

    /* commit exactly one 4 KB page in the middle of the first 16 KB host page */
    b2 = base + 0x1000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_COMMIT, P4K_PAGE_READONLY ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ );
    CHECK_EQ( mock_prot_at( m, base + 0x4000 ), 0 );

    /* Windows sees exactly three 4 KB-granular regions */
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_RESERVE ); CHECK_EQ( mbi.region_size, 0x1000u );
    CHECK_EQ( p4k_space_query( s, base + 0x1000, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_COMMIT ); CHECK_EQ( mbi.region_size, 0x1000u );
    CHECK_EQ( mbi.protect, P4K_PAGE_READONLY ); CHECK_EQ( mbi.base, base + 0x1000 );
    CHECK_EQ( p4k_space_query( s, base + 0x1800, &mbi ), P4K_OK );   /* mid-page address */
    CHECK_EQ( mbi.base, base + 0x1000 );
    CHECK_EQ( p4k_space_query( s, base + 0x2000, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_RESERVE ); CHECK_EQ( mbi.region_size, 0xe000u );

    /* VirtualProtect on the single 4 KB page returns the exact old protection */
    b2 = base + 0x1000; s2 = 0x1000;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READWRITE, &old ), P4K_OK );
    CHECK_EQ( old, P4K_PAGE_READONLY );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );
    /* protecting a reserved page fails */
    b2 = base + 0x2000; s2 = 0x1000;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READONLY, &old ), P4K_E_NOT_COMMITTED );
    CHECK_EQ( old, P4K_PAGE_NOACCESS );
    /* range spanning committed + reserved fails too, and changes nothing */
    b2 = base + 0x1000; s2 = 0x2000;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READONLY, &old ), P4K_E_NOT_COMMITTED );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );

    /* adjacent pages with different protections inside one host page */
    b2 = base + 0x2000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_COMMIT, P4K_PAGE_EXECUTE_READ ), P4K_OK );
    CHECK_EQ( p4k_space_query( s, base + 0x1000, &mbi ), P4K_OK );
    CHECK_EQ( mbi.protect, P4K_PAGE_READWRITE ); CHECK_EQ( mbi.region_size, 0x1000u );
    CHECK_EQ( p4k_space_query( s, base + 0x2000, &mbi ), P4K_OK );
    CHECK_EQ( mbi.protect, P4K_PAGE_EXECUTE_READ ); CHECK_EQ( mbi.region_size, 0x1000u );
    /* RW + guest RX in one host page: RW on the host, X dropped, no W|X */
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );

    /* release */
    CHECK_EQ( p4k_space_release( s, base + 0x1000 ), P4K_E_NOT_AT_BASE );
    CHECK_EQ( p4k_space_release( s, base ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), -1 );
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_FREE );
    CHECK_EQ( p4k_space_release( s, base ), P4K_E_NOT_ALLOCATED );
    done( s, m );
}

static void test_decommit_zeroes( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0, a;
    size_t size = 0x10000, sz2;
    uint8_t v;
    p4k_mbi mbi;

    printf( "decommit_zeroes\n" );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT, P4K_PAGE_READWRITE ), P4K_OK );
    for (a = base; a < base + 0x8000; a += 0x1000) CHECK( mock_write8( m, a, 0xA0 + (uint8_t)((a - base) >> 12) ) );

    /* decommit the 2nd 4 KB page only: its bytes must be zero, neighbours intact, host page stays RW */
    CHECK_EQ( p4k_space_decommit( s, base + 0x1000, 0x1000 ), P4K_OK );
    CHECK( mock_read8( m, base + 0x1000, &v ) ); CHECK_EQ( v, 0 );
    CHECK( mock_read8( m, base, &v ) );          CHECK_EQ( v, 0xA0 );
    CHECK( mock_read8( m, base + 0x2000, &v ) ); CHECK_EQ( v, 0xA2 );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK_EQ( p4k_space_query( s, base + 0x1000, &mbi ), P4K_OK );
    CHECK_EQ( mbi.state, P4K_MEM_RESERVE ); CHECK_EQ( mbi.region_size, 0x1000u );

    /* decommit the rest of the first host page: it goes NONE and zero */
    CHECK_EQ( p4k_space_decommit( s, base, 0x1000 ), P4K_OK );
    CHECK_EQ( p4k_space_decommit( s, base + 0x2000, 0x2000 ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), 0 );

    /* the second host page (a distinct 16 KB) was untouched: still data, still RW */
    CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK( mock_read8( m, base + 0x4000, &v ) ); CHECK_EQ( v, 0xA4 );

    /* recommit one page of the first host page (RO): reads zero, even though stale bytes were written before */
    sz2 = 0x1000; a = base + 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &a, &sz2, P4K_MEM_COMMIT, P4K_PAGE_READONLY ), P4K_OK );
    CHECK( mock_read8( m, base + 0x1000, &v ) ); CHECK_EQ( v, 0 );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ );

    /* whole-allocation decommit (size 0) needs the base */
    CHECK_EQ( p4k_space_decommit( s, base + 0x1000, 0 ), P4K_E_NOT_AT_BASE );
    CHECK_EQ( p4k_space_decommit( s, base, 0 ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base + 0x4000 ), 0 );
    CHECK_EQ( p4k_space_decommit( s, base, 0x20000 ), P4K_E_UNABLE_TO_FREE );
    done( s, m );
}

static void test_guard_and_writewatch( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0, b2;
    size_t size = 0x10000, s2;
    p4k_mbi mbi;
    uintptr_t addrs[32];
    size_t count;
    uint32_t old;

    printf( "guard_and_writewatch (soft)\n" );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT,
                               P4K_PAGE_READWRITE | P4K_PAGE_GUARD ), P4K_OK );
    /* queries report the guard bit and AllocationProtect carries it */
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );
    CHECK_EQ( mbi.protect, P4K_PAGE_READWRITE | P4K_PAGE_GUARD );
    CHECK_EQ( mbi.alloc_protect, P4K_PAGE_READWRITE | P4K_PAGE_GUARD );
    /* but the host stays accessible: no deliberate fault */
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );
    /* accessing it does not clear the guard (one-shot semantics are not reproduced) */
    CHECK( mock_write8( m, base, 1 ) );
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );
    CHECK_EQ( mbi.protect, P4K_PAGE_READWRITE | P4K_PAGE_GUARD );
    /* VirtualProtect clears it explicitly and returns the old value including the guard bit */
    b2 = base; s2 = 0x4000;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READWRITE, &old ), P4K_OK );
    CHECK_EQ( old, P4K_PAGE_READWRITE | P4K_PAGE_GUARD );
    CHECK_EQ( p4k_space_release( s, base ), P4K_OK );

    printf( "  write watch\n" );
    base = 0; size = 0x10000;
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT | P4K_MEM_WRITE_WATCH,
                               P4K_PAGE_READWRITE ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );
    count = 32;
    CHECK_EQ( p4k_space_get_write_watch( s, 0, base, 0x10000, addrs, &count ), P4K_OK );
    CHECK_EQ( count, 16u );             /* conservative: every page reported */
    CHECK_EQ( addrs[0], base ); CHECK_EQ( addrs[15], base + 15 * 0x1000 );
    count = 4;
    CHECK_EQ( p4k_space_get_write_watch( s, 1, base + 0x3000, 0x8000, addrs, &count ), P4K_OK );
    CHECK_EQ( count, 4u );              /* capped by the caller's buffer */
    count = 32;
    CHECK_EQ( p4k_space_get_write_watch( s, 0, base + 0x3000, 0x2000, addrs, &count ), P4K_OK );
    CHECK_EQ( count, 2u );              /* reset did not hide anything */
    count = 32;
    CHECK_EQ( p4k_space_get_write_watch( s, 0, 0x1000, 0x2000, addrs, &count ), P4K_E_INVALID_PARAMETER );
    done( s, m );

    /* fault mode (Linux/Asahi, no debugger): watched host page is RO until written */
    {
        p4k_space *f = mk( &m, 16384, P4K_POLICY_DROP_GUEST_X | P4K_POLICY_ENFORCE_WX );
        base = 0; size = 0x10000;
        CHECK_EQ( p4k_space_alloc( f, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT | P4K_MEM_WRITE_WATCH,
                                   P4K_PAGE_READWRITE ), P4K_OK );
        CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ );
        count = 32;
        CHECK_EQ( p4k_space_get_write_watch( f, 0, base, 0x10000, addrs, &count ), P4K_OK );
        CHECK_EQ( count, 0u );
        CHECK_EQ( p4k_space_note_write( f, base + 0x5000 ), P4K_OK );    /* fault handler path */
        CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ | P4K_HOST_WRITE );
        CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ );
        count = 32;
        CHECK_EQ( p4k_space_get_write_watch( f, 1, base, 0x10000, addrs, &count ), P4K_OK );
        CHECK_EQ( count, 4u );          /* whole 16 KB host page is reported: the documented coarsening */
        CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ );
        done( f, m );
    }
}

static void test_image_two_pass( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0;
    p4k_mbi mbi;
    unsigned i;
    /* x86-64 style layout: 4 KB section alignment, header 1 page, .text RX 5 pages, .rdata R 2, .data RW 3 */
    p4k_image_section sec[3] = {
        { 0x1000, 0x5000, 0x60000020, 0 },   /* .text  R+X  */
        { 0x6000, 0x2000, 0x40000040, 0 },   /* .rdata R    */
        { 0x8000, 0x3000, 0xC0000040, 0 },   /* .data  R+W  */
    };

    printf( "image_two_pass\n" );
    CHECK_EQ( p4k_space_image_begin( s, &base, 0xB000 ), P4K_OK );
    /* pass 1: a single RW mprotect, never X */
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_WRITE );
    mock_reset_calls( m );
    CHECK_EQ( p4k_space_image_finalize( s, base, 0x400, sec, 3 ), P4K_OK );
    for (i = 0; i < m->ncalls && i < MOCK_MAX_CALLS; i++)
        if (m->calls[i].kind == MC_MPROTECT) CHECK( !((m->calls[i].prot & 2) && (m->calls[i].prot & 4)) );
    /* final protections as Windows sees them (4 KB exact) */
    CHECK_EQ( p4k_space_query( s, base, &mbi ), P4K_OK );          CHECK_EQ( mbi.protect, P4K_PAGE_READONLY );          CHECK_EQ( mbi.region_size, 0x1000u );
    CHECK_EQ( p4k_space_query( s, base + 0x1000, &mbi ), P4K_OK ); CHECK_EQ( mbi.protect, P4K_PAGE_EXECUTE_READ );     CHECK_EQ( mbi.region_size, 0x5000u );
    CHECK_EQ( p4k_space_query( s, base + 0x6000, &mbi ), P4K_OK ); CHECK_EQ( mbi.protect, P4K_PAGE_READONLY );          CHECK_EQ( mbi.region_size, 0x2000u );
    CHECK_EQ( p4k_space_query( s, base + 0x8000, &mbi ), P4K_OK ); CHECK_EQ( mbi.protect, P4K_PAGE_WRITECOPY );        CHECK_EQ( mbi.region_size, 0x3000u );
    CHECK_EQ( mbi.type, P4K_MEM_IMAGE ); CHECK_EQ( mbi.alloc_protect, P4K_PAGE_EXECUTE_WRITECOPY );
    /* host: page 0 = header + .text (guest X dropped) -> R; page 1 = .text tail + .rdata -> R;
     * page 2 = .data -> RW.  Nothing is executable: guest code is only ever read by the emulator. */
    CHECK_EQ( mock_prot_at( m, base ),          P4K_HOST_READ );
    CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ );
    CHECK_EQ( mock_prot_at( m, base + 0x8000 ), P4K_HOST_READ | P4K_HOST_WRITE );
    done( s, m );
}

static void test_image_native_sections( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0;
    /* ARM64-style layout: sections aligned to the host page size, native code */
    p4k_image_section sec[3] = {
        { 0x4000, 0x4000, 0x60000020, 1 },   /* .text  R+X native */
        { 0x8000, 0x4000, 0x40000040, 0 },   /* .rdata R */
        { 0xC000, 0x4000, 0xC0000040, 0 },   /* .data  R+W */
    };

    printf( "image_native_sections\n" );
    CHECK_EQ( p4k_space_image_begin( s, &base, 0x10000 ), P4K_OK );
    CHECK_EQ( p4k_space_image_finalize( s, base, 0x4000, sec, 3 ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ),          P4K_HOST_READ );
    CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ | P4K_HOST_EXEC );
    CHECK_EQ( mock_prot_at( m, base + 0x8000 ), P4K_HOST_READ );
    CHECK_EQ( mock_prot_at( m, base + 0xC000 ), P4K_HOST_READ | P4K_HOST_WRITE );
    CHECK_EQ( m->conflicts, 0u );
    done( s, m );

    /* native code that shares a 16 KB host page with writable data cannot be both: X is dropped and the
     * conflict is reported, so a first run on a device shows exactly which page it was */
    {
        p4k_image_section bad[2] = {
            { 0x4000, 0x2000, 0x60000020, 1 },   /* native .text */
            { 0x6000, 0x2000, 0xC0000040, 0 },   /* .data in the same host page */
        };
        s = mk( &m, 16384, F_IOS );
        base = 0;
        CHECK_EQ( p4k_space_image_begin( s, &base, 0x8000 ), P4K_OK );
        CHECK_EQ( p4k_space_image_finalize( s, base, 0x4000, bad, 2 ), P4K_OK );
        CHECK_EQ( mock_prot_at( m, base + 0x4000 ), P4K_HOST_READ | P4K_HOST_WRITE );
        CHECK( m->conflicts >= 1 );   /* reported each time the page is (re)applied */
        done( s, m );
    }
}

static void test_linux_passthrough( void )
{
    /* policy 0 = what stock Wine does today on a 16 KB Linux host: X passes through */
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, 0 );
    uintptr_t base = 0;
    size_t size = 0x4000;

    printf( "linux_passthrough\n" );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT, P4K_PAGE_EXECUTE_READ ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), P4K_HOST_READ | P4K_HOST_EXEC );
    done( s, m );
}

static void test_errors( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 16384, F_IOS );
    uintptr_t base = 0, b2;
    size_t size = 0x8000, s2;
    uint32_t old;

    printf( "errors\n" );
    s2 = 0;
    CHECK_EQ( p4k_space_alloc( s, &base, &s2, P4K_MEM_RESERVE, P4K_PAGE_READWRITE ), P4K_E_INVALID_PARAMETER );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, 0, P4K_PAGE_READWRITE ), P4K_E_INVALID_PARAMETER );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE, 0x3 ), P4K_E_INVALID_PAGE_PROTECTION );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE, P4K_PAGE_WRITECOPY ), P4K_E_INVALID_PAGE_PROTECTION );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_COMMIT | P4K_MEM_WRITE_WATCH, P4K_PAGE_READWRITE ), P4K_E_INVALID_PARAMETER );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE | P4K_MEM_COMMIT, P4K_PAGE_READWRITE ), P4K_OK );
    /* fixed reserve over it conflicts; commit outside any allocation is "not mapped" */
    b2 = base + 0x4000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_RESERVE, P4K_PAGE_READWRITE ), P4K_E_CONFLICT );
    b2 = base + 0x100000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_COMMIT, P4K_PAGE_READWRITE ), P4K_E_NOT_MAPPED );
    b2 = base + 0x1000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_COMMIT, P4K_PAGE_WRITECOPY ), P4K_E_INVALID_PAGE_PROTECTION );
    s2 = 0;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READONLY, &old ), P4K_E_INVALID_PARAMETER );
    b2 = 0x20000000; s2 = 0x1000;
    CHECK_EQ( p4k_space_protect( s, &b2, &s2, P4K_PAGE_READONLY, &old ), P4K_E_INVALID_PARAMETER );
    done( s, m );
}

/* a 4 KB host (no larger pages) must behave exactly like a plain per-page implementation */
static void test_4k_host_is_identity( void )
{
    struct mock_host *m;
    p4k_space *s = mk( &m, 4096, F_IOS );
    uintptr_t base = 0;
    size_t size = 0x4000;
    uintptr_t b2;
    size_t s2;

    printf( "4k_host_is_identity\n" );
    CHECK_EQ( p4k_space_alloc( s, &base, &size, P4K_MEM_RESERVE, P4K_PAGE_READWRITE ), P4K_OK );
    b2 = base + 0x2000; s2 = 0x1000;
    CHECK_EQ( p4k_space_alloc( s, &b2, &s2, P4K_MEM_COMMIT, P4K_PAGE_READWRITE ), P4K_OK );
    CHECK_EQ( mock_prot_at( m, base ), 0 );
    CHECK_EQ( mock_prot_at( m, base + 0x2000 ), 3 );
    CHECK_EQ( mock_prot_at( m, base + 0x3000 ), 0 );
    done( s, m );
}

int main( void )
{
    test_policy_and_conversion();
    test_union();
    test_equals_stock_wine();
    test_apply_range_coalescing();
    test_space_basic_4k_semantics();
    test_decommit_zeroes();
    test_guard_and_writewatch();
    test_image_two_pass();
    test_image_native_sections();
    test_linux_passthrough();
    test_errors();
    test_4k_host_is_identity();
    if (g_failures) { printf( "FAILED: %d check(s)\n", g_failures ); return 1; }
    printf( "test_unit: all passed\n" );
    return 0;
}
