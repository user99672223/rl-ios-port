/*
 * vm4k.exe - synthetic Windows program that checks 4 KB page semantics from the guest's side:
 * VirtualAlloc / VirtualProtect / VirtualQuery / VirtualFree / GetWriteWatch and the protections
 * of its own PE image, with every protection change made on single 4 KB pages that share
 * larger host pages.  It contains no game code.  Build (any Linux host):
 *     x86_64-w64-mingw32-gcc -O1 -Wall -o vm4k.exe vm4k.c
 * Run under Wine (any host page size); exit status 0 = all checks passed.  The guard-page and
 * write-watch sections print what they observed; the asserted behaviour is the part both the
 * stock and the soft (no-fault) implementations must share.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf( "FAIL line %d: %s\n", __LINE__, #c ); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); checks++; \
    if (_a != _b) { failures++; printf( "FAIL line %d: %s == %s (got %#llx, want %#llx)\n", __LINE__, #a, #b, _a, _b ); } } while (0)

static MEMORY_BASIC_INFORMATION q( void *p )
{
    MEMORY_BASIC_INFORMATION m;
    memset( &m, 0, sizeof(m) );
    VirtualQuery( p, &m, sizeof(m) );
    return m;
}

static void test_system( void )
{
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    CHECK_EQ( si.dwPageSize, 0x1000 );
    CHECK_EQ( si.dwAllocationGranularity, 0x10000 );
}

static void test_regions( void )
{
    char *p = VirtualAlloc( NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE );
    MEMORY_BASIC_INFORMATION m;
    DWORD old;
    int i;

    CHECK( p != NULL );
    if (!p) return;
    CHECK_EQ( (ULONG_PTR)p & 0xffff, 0 );
    m = q( p );
    CHECK_EQ( m.State, MEM_RESERVE ); CHECK_EQ( m.RegionSize, 0x10000 ); CHECK_EQ( m.AllocationBase, p );
    CHECK_EQ( m.AllocationProtect, PAGE_READWRITE ); CHECK_EQ( m.Type, MEM_PRIVATE ); CHECK_EQ( m.BaseAddress, p );

    /* commit single 4 KB pages with different protections, all inside the first 16 KB */
    CHECK( VirtualAlloc( p + 0x1000, 0x1000, MEM_COMMIT, PAGE_READONLY ) == p + 0x1000 );
    CHECK( VirtualAlloc( p + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE ) == p + 0x2000 );
    m = q( p );          CHECK_EQ( m.State, MEM_RESERVE ); CHECK_EQ( m.RegionSize, 0x1000 );
    m = q( p + 0x1000 ); CHECK_EQ( m.State, MEM_COMMIT ); CHECK_EQ( m.Protect, PAGE_READONLY ); CHECK_EQ( m.RegionSize, 0x1000 );
    m = q( p + 0x1abc ); CHECK_EQ( m.BaseAddress, p + 0x1000 );
    m = q( p + 0x2000 ); CHECK_EQ( m.State, MEM_COMMIT ); CHECK_EQ( m.Protect, PAGE_READWRITE ); CHECK_EQ( m.RegionSize, 0x1000 );
    m = q( p + 0x3000 ); CHECK_EQ( m.State, MEM_RESERVE ); CHECK_EQ( m.RegionSize, 0xd000 );

    /* the committed RW page is really writable, its neighbours keep their 4 KB protections for the guest */
    memset( p + 0x2000, 0x5a, 0x1000 );
    CHECK( VirtualProtect( p + 0x1000, 0x1000, PAGE_READWRITE, &old ) && old == PAGE_READONLY );
    m = q( p + 0x1000 ); CHECK_EQ( m.Protect, PAGE_READWRITE ); CHECK_EQ( m.RegionSize, 0x2000 );   /* now one region */
    CHECK( VirtualProtect( p + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old ) && old == PAGE_READWRITE );
    m = q( p + 0x1000 ); CHECK_EQ( m.Protect, PAGE_EXECUTE_READ ); CHECK_EQ( m.RegionSize, 0x1000 );
    CHECK( VirtualProtect( p + 0x2000, 0x1000, PAGE_NOACCESS, &old ) && old == PAGE_READWRITE );
    m = q( p + 0x2000 ); CHECK_EQ( m.Protect, PAGE_NOACCESS ); CHECK_EQ( m.State, MEM_COMMIT );
    CHECK( VirtualProtect( p + 0x2000, 0x1000, PAGE_READWRITE, &old ) && old == PAGE_NOACCESS );
    CHECK_EQ( (unsigned char)p[0x2000], 0x5a );          /* data survived the protection changes */
    CHECK_EQ( (unsigned char)p[0x2fff], 0x5a );

    /* protecting a range that includes a reserved page fails and changes nothing */
    SetLastError( 0 );
    CHECK( !VirtualProtect( p + 0x2000, 0x2000, PAGE_READONLY, &old ) );
    CHECK_EQ( GetLastError(), ERROR_INVALID_ADDRESS );   /* STATUS_NOT_COMMITTED */
    m = q( p + 0x2000 ); CHECK_EQ( m.Protect, PAGE_READWRITE );

    /* a whole 16 KB worth of single-page protections cycled through every kind */
    CHECK( VirtualAlloc( p + 0x3000, 0x9000, MEM_COMMIT, PAGE_READWRITE ) != NULL );
    {
        static const DWORD prots[] = { PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE, PAGE_EXECUTE_READ,
                                       PAGE_EXECUTE_READWRITE, PAGE_NOACCESS };
        for (i = 0; i < 9; i++)
        {
            DWORD want = prots[i % 6];
            CHECK( VirtualProtect( p + 0x3000 + i * 0x1000, 0x1000, want, &old ) );
            m = q( p + 0x3000 + i * 0x1000 );
            CHECK_EQ( m.Protect, want );
            CHECK_EQ( m.BaseAddress, p + 0x3000 + i * 0x1000 - (i && prots[(i - 1) % 6] == want ? 0x1000 : 0) );
        }
        for (i = 0; i < 9; i++)
        {
            m = q( p + 0x3000 + i * 0x1000 );
            CHECK_EQ( m.Protect, prots[i % 6] );         /* earlier pages unaffected by later changes */
            CHECK_EQ( m.RegionSize, 0x1000 );            /* all neighbours differ: exactly one 4 KB page each */
        }
    }
    CHECK( VirtualFree( p + 0x1000, 0, MEM_RELEASE ) == 0 );        /* not the allocation base */
    CHECK( VirtualFree( p, 0, MEM_RELEASE ) );
    m = q( p );          CHECK_EQ( m.State, MEM_FREE );
}

static void test_decommit_zero( void )
{
    char *p = VirtualAlloc( NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    int i;

    CHECK( p != NULL );
    if (!p) return;
    for (i = 0; i < 8; i++) memset( p + i * 0x1000, 0xa0 + i, 0x1000 );

    CHECK( VirtualFree( p + 0x1000, 0x1000, MEM_DECOMMIT ) );          /* middle of a host page */
    CHECK_EQ( q( p + 0x1000 ).State, MEM_RESERVE );
    CHECK_EQ( q( p + 0x1000 ).RegionSize, 0x1000 );
    CHECK_EQ( (unsigned char)p[0], 0xa0 );                            /* neighbours untouched */
    CHECK_EQ( (unsigned char)p[0x2000], 0xa2 );
    CHECK( VirtualFree( p + 0x5000, 0x2000, MEM_DECOMMIT ) );          /* crosses a host page boundary */
    CHECK_EQ( (unsigned char)p[0x4fff], 0xa4 );
    CHECK_EQ( (unsigned char)p[0x7000], 0xa7 );

    CHECK( VirtualAlloc( p + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE ) == p + 0x1000 );
    CHECK( VirtualAlloc( p + 0x5000, 0x2000, MEM_COMMIT, PAGE_READWRITE ) == p + 0x5000 );
    for (i = 0; i < 0x1000; i += 0x7f)
    {
        CHECK_EQ( (unsigned char)p[0x1000 + i], 0 );                  /* zero-filled again */
        CHECK_EQ( (unsigned char)p[0x5000 + i], 0 );
        CHECK_EQ( (unsigned char)p[0x6000 + i], 0 );
    }
    CHECK_EQ( (unsigned char)p[0x1fff], 0 );
    CHECK_EQ( (unsigned char)p[0x6fff], 0 );
    CHECK_EQ( (unsigned char)p[0], 0xa0 );
    CHECK_EQ( (unsigned char)p[0x4fff], 0xa4 );
    CHECK_EQ( (unsigned char)p[0x7000], 0xa7 );

    /* whole-allocation decommit of a size that is not a multiple of any larger page */
    CHECK( VirtualFree( p, 0, MEM_DECOMMIT ) );
    CHECK_EQ( q( p ).State, MEM_RESERVE );
    CHECK_EQ( q( p ).RegionSize, 0x10000 );
    CHECK( VirtualFree( p, 0, MEM_RELEASE ) );
    {
        char *s = VirtualAlloc( NULL, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        CHECK( s != NULL );
        if (s)
        {
            s[0] = 1;
            CHECK( VirtualFree( s, 0, MEM_DECOMMIT ) );
            CHECK( VirtualAlloc( s, 0x3000, MEM_COMMIT, PAGE_READWRITE ) == s );
            CHECK_EQ( s[0], 0 );
            CHECK( VirtualFree( s, 0, MEM_RELEASE ) );
        }
    }
}

static void test_guard_and_watch( void )
{
    char *p = VirtualAlloc( NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    MEMORY_BASIC_INFORMATION m;
    DWORD old;
    void *pages[64];
    ULONG_PTR count;
    DWORD gran;
    UINT rc;

    CHECK( p != NULL );
    if (!p) return;
    /* a guard on ONE 4 KB page: the query must report it exactly, whatever the host does */
    CHECK( VirtualProtect( p + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old ) && old == PAGE_READWRITE );
    m = q( p + 0x1000 ); CHECK_EQ( m.Protect, PAGE_READWRITE | PAGE_GUARD ); CHECK_EQ( m.RegionSize, 0x1000 );
    m = q( p );          CHECK_EQ( m.Protect, PAGE_READWRITE ); CHECK_EQ( m.RegionSize, 0x1000 );
    CHECK( VirtualProtect( p + 0x1000, 0x1000, PAGE_READWRITE, &old ) && old == (PAGE_READWRITE | PAGE_GUARD) );
    VirtualFree( p, 0, MEM_RELEASE );

    p = VirtualAlloc( NULL, 0x8000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE );
    CHECK( p != NULL );
    if (!p) return;
    count = 64;
    rc = GetWriteWatch( 0, p, 0x8000, pages, &count, &gran );
    CHECK_EQ( rc, 0 );
    printf( "  write watch before any write: %lu of 8 pages reported (0 = armed by faults, 8 = conservative / no-fault)\n",
            (unsigned long)count );
    p[0x2000] = 1; p[0x5000] = 2;
    count = 64;
    rc = GetWriteWatch( 0, p, 0x8000, pages, &count, &gran );
    CHECK_EQ( rc, 0 );
    CHECK_EQ( gran, 0x1000 );
    {
        int saw2 = 0, saw5 = 0;
        ULONG_PTR i;
        for (i = 0; i < count; i++)
        {
            if (pages[i] == p + 0x2000) saw2 = 1;
            if (pages[i] == p + 0x5000) saw5 = 1;
        }
        CHECK( saw2 && saw5 );                      /* never miss a written page */
        printf( "  write watch after two writes: %lu of 8 pages reported\n", (unsigned long)count );
    }
    VirtualFree( p, 0, MEM_RELEASE );
}

/* the protections of this very executable, 4 KB page by 4 KB page, against its section headers */
static void test_image( void )
{
    HMODULE mod = GetModuleHandleW( NULL );
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
    MEMORY_BASIC_INFORMATION m;
    unsigned i;
    int pages_checked = 0;

    m = q( mod );
    CHECK_EQ( m.Type, MEM_IMAGE );
    CHECK_EQ( m.AllocationBase, mod );
    CHECK_EQ( m.Protect, PAGE_READONLY );          /* headers */
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        DWORD ch = sec[i].Characteristics, want;
        int r = (ch & IMAGE_SCN_MEM_READ) != 0, w = (ch & IMAGE_SCN_MEM_WRITE) != 0, x = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
        DWORD size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        DWORD off;

        want = w ? (x ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY)
                 : x ? (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                     : (r ? PAGE_READONLY : PAGE_NOACCESS);
        for (off = 0; off < ((size + 0xfff) & ~0xfffu); off += 0x1000)
        {
            m = q( (char *)mod + sec[i].VirtualAddress + off );
            if (m.Protect != want)
            {
                printf( "  section %.8s +%#lx: protect %#lx, expected %#lx\n", sec[i].Name, (unsigned long)off,
                        (unsigned long)m.Protect, (unsigned long)want );
                failures++;
            }
            checks++;
            pages_checked++;
        }
    }
    printf( "  %d image pages in %u sections checked at 4 KB granularity\n", pages_checked, nt->FileHeader.NumberOfSections );
}

int main( void )
{
    printf( "vm4k: start\n" );
    test_system();
    test_regions();
    test_decommit_zero();
    test_guard_and_watch();
    test_image();
    printf( "vm4k: %d checks, %d failures\n", checks, failures );
    printf( failures ? "vm4k: FAIL\n" : "vm4k: PASS\n" );
    return failures != 0;
}
