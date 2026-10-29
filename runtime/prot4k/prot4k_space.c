/*
 * prot4k_space - address-space model on top of the prot4k core. See prot4k_space.h.
 *
 * The control flow of each operation follows Wine's dlls/ntdll/unix/virtual.c so that this is
 * a faithful, testable stand-in for the Wine code paths (allocate_virtual_memory, set_protection,
 * decommit_pages, free_pages, fill_basic_memory_info, map_image_into_view).
 */
#include <stdlib.h>
#include <string.h>
#include "prot4k_space.h"

#define GRANULARITY ((uintptr_t)0x10000)

enum { KIND_PRIVATE, KIND_IMAGE };

struct alloc
{
    struct alloc *next;        /* sorted by base */
    uintptr_t     base;
    size_t        size;        /* multiple of 4 KB */
    size_t        host_size;   /* size rounded up to the host page size */
    uint32_t      alloc_protect;
    int           kind;
    int           write_watch;
    uint8_t      *pages;       /* size / 4 KB part bytes */
};

struct p4k_space
{
    p4k_policy     pol;
    p4k_space_host host;
    uintptr_t      win_base;
    size_t         win_size;
    struct alloc  *list;
    struct alloc  *cache;      /* last lookup, for get_part */
};

static uintptr_t round_down( uintptr_t v, uintptr_t m ) { return v & ~(m - 1); }
static uintptr_t round_up( uintptr_t v, uintptr_t m ) { return (v + m - 1) & ~(m - 1); }

static struct alloc *find_alloc( p4k_space *s, uintptr_t addr )
{
    struct alloc *a;

    if (s->cache && addr >= s->cache->base && addr - s->cache->base < s->cache->size) return s->cache;
    for (a = s->list; a; a = a->next)
    {
        if (addr < a->base) break;
        if (addr - a->base < a->size) return s->cache = a;
    }
    return NULL;
}

static uint8_t get_part( void *ud, uintptr_t page )
{
    p4k_space *s = ud;
    uintptr_t addr = page << P4K_PAGE_SHIFT;
    struct alloc *a = find_alloc( s, addr );

    return a ? a->pages[(addr - a->base) >> P4K_PAGE_SHIFT] : 0;
}

uint8_t p4k_space_part( p4k_space *s, uintptr_t addr )
{
    return get_part( s, addr >> P4K_PAGE_SHIFT );
}

p4k_space *p4k_space_new( const p4k_policy *p, const p4k_space_host *host, uintptr_t win_base, size_t win_size )
{
    p4k_space *s;

    if (p4k_policy_check( p ) || (win_base & (GRANULARITY - 1)) || (win_size & (GRANULARITY - 1))) return NULL;
    if (p->host_page_size > GRANULARITY) return NULL;
    if (!(s = calloc( 1, sizeof(*s) ))) return NULL;
    s->pol = *p;
    s->host = *host;
    s->win_base = win_base;
    s->win_size = win_size;
    return s;
}

void p4k_space_free( p4k_space *s )
{
    struct alloc *a, *next;

    for (a = s->list; a; a = next)
    {
        next = a->next;
        free( a->pages );
        free( a );
    }
    free( s );
}

static int in_window( p4k_space *s, uintptr_t base, size_t size )
{
    return base >= s->win_base && size <= s->win_size && base - s->win_base <= s->win_size - size;
}

/* host range [base, base+host_size) of a new allocation must not overlap any existing one */
static int host_overlap( p4k_space *s, uintptr_t base, size_t size )
{
    const uintptr_t hs = s->pol.host_page_size;
    uintptr_t end = round_up( base + size, hs );
    struct alloc *a;

    for (a = s->list; a; a = a->next)
    {
        uintptr_t a_end = a->base + a->host_size;
        if (a->base < end && base < a_end) return 1;
    }
    return 0;
}

static void insert_alloc( p4k_space *s, struct alloc *n )
{
    struct alloc **pp = &s->list;

    while (*pp && (*pp)->base < n->base) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    s->cache = NULL;
}

static int find_free( p4k_space *s, size_t size, uintptr_t *out )
{
    const uintptr_t hs = s->pol.host_page_size;
    size_t need = round_up( size, hs );
    uintptr_t cur = s->win_base;
    struct alloc *a;

    for (a = s->list; ; a = a->next)
    {
        uintptr_t limit = a ? a->base : s->win_base + s->win_size;
        if (limit >= cur && limit - cur >= need) { *out = cur; return 0; }
        if (!a) return -1;
        cur = round_up( a->base + a->host_size, GRANULARITY );
    }
}

static int apply( p4k_space *s, uintptr_t base, size_t size )
{
    return p4k_apply_range( &s->pol, get_part, s, &s->host.ops, base, size, 0, 0 ) ? P4K_E_HOST : P4K_OK;
}

static int create_alloc( p4k_space *s, uintptr_t base, size_t size, int kind, uint32_t alloc_protect,
                         uint8_t init, int write_watch, struct alloc **ret )
{
    const uintptr_t hs = s->pol.host_page_size;
    struct alloc *a;
    size_t i;

    if (!(a = calloc( 1, sizeof(*a) ))) return P4K_E_NO_MEMORY;
    if (!(a->pages = malloc( size >> P4K_PAGE_SHIFT )))
    {
        free( a );
        return P4K_E_NO_MEMORY;
    }
    a->base = base;
    a->size = size;
    a->host_size = round_up( size, hs );
    a->alloc_protect = alloc_protect;
    a->kind = kind;
    a->write_watch = write_watch;
    if (write_watch && !(s->pol.flags & P4K_POLICY_SOFT_WRITEWATCH)) init |= P4K_WRITEWATCH;
    for (i = 0; i < size >> P4K_PAGE_SHIFT; i++) a->pages[i] = init;

    if (s->host.map( s->host.ops.ud, base, a->host_size ))
    {
        free( a->pages );
        free( a );
        return P4K_E_NO_MEMORY;
    }
    insert_alloc( s, a );
    *ret = a;
    return P4K_OK;
}

/* set the protection of a committed-or-not range, keeping the private bits (write watch, native) */
static void set_protection_bits( struct alloc *a, uintptr_t base, size_t size, uint8_t vprot )
{
    size_t i, first = (base - a->base) >> P4K_PAGE_SHIFT, n = size >> P4K_PAGE_SHIFT;

    for (i = 0; i < n; i++)
        a->pages[first + i] = vprot | (a->pages[first + i] & P4K_PRIVATE_BITS);
}

static int set_protection( p4k_space *s, struct alloc *a, uintptr_t base, size_t size, uint32_t protect )
{
    uint8_t vprot;

    if (p4k_vprot_from_win32( protect, a->kind == KIND_IMAGE, &vprot )) return P4K_E_INVALID_PAGE_PROTECTION;
    if (a->kind == KIND_PRIVATE)
    {
        if (vprot & P4K_WRITECOPY) return P4K_E_INVALID_PAGE_PROTECTION;
    }
    set_protection_bits( a, base, size, vprot | P4K_COMMITTED );
    return apply( s, base, size );
}

int p4k_space_alloc( p4k_space *s, uintptr_t *base_ptr, size_t *size_ptr, uint32_t type, uint32_t protect )
{
    const uint32_t mask = P4K_MEM_COMMIT | P4K_MEM_RESERVE | P4K_MEM_WRITE_WATCH;
    uintptr_t base = *base_ptr;
    size_t size = *size_ptr;
    struct alloc *a;
    uint8_t vprot;
    int ret;

    if (!size) return P4K_E_INVALID_PARAMETER;
    if (type & ~mask) return P4K_E_INVALID_PARAMETER;
    if (!(type & (P4K_MEM_COMMIT | P4K_MEM_RESERVE))) return P4K_E_INVALID_PARAMETER;
    if ((type & P4K_MEM_WRITE_WATCH) && !(type & P4K_MEM_RESERVE)) return P4K_E_INVALID_PARAMETER;

    if (base)
    {
        uintptr_t end = base + size;
        if (end < base) return P4K_E_INVALID_PARAMETER;
        base = (type & P4K_MEM_RESERVE) ? round_down( base, GRANULARITY ) : round_down( base, P4K_PAGE_SIZE );
        size = round_up( end, P4K_PAGE_SIZE ) - base;
    }
    else size = round_up( size, P4K_PAGE_SIZE );

    if ((type & P4K_MEM_RESERVE) || !base)
    {
        if (p4k_vprot_from_win32( protect, 0, &vprot )) return P4K_E_INVALID_PAGE_PROTECTION;
        if (vprot & P4K_WRITECOPY) return P4K_E_INVALID_PAGE_PROTECTION;
        if (type & P4K_MEM_COMMIT) vprot |= P4K_COMMITTED;

        if (base)
        {
            if (!in_window( s, base, size )) return P4K_E_INVALID_PARAMETER;
            if (host_overlap( s, base, size )) return P4K_E_CONFLICT;
        }
        else if (find_free( s, size, &base )) return P4K_E_NO_MEMORY;

        if ((ret = create_alloc( s, base, size, KIND_PRIVATE, p4k_win32_from_vprot( vprot & ~P4K_COMMITTED ),
                                 vprot, (type & P4K_MEM_WRITE_WATCH) != 0, &a ))) return ret;
        if ((ret = apply( s, base, size ))) return ret;
    }
    else  /* commit pages inside an existing allocation */
    {
        if (!(a = find_alloc( s, base )) || base + size > a->base + a->size) return P4K_E_NOT_MAPPED;
        if (a->kind != KIND_PRIVATE) return P4K_E_INVALID_PARAMETER;
        if ((ret = set_protection( s, a, base, size, protect ))) return ret;
    }
    *base_ptr = base;
    *size_ptr = size;
    return P4K_OK;
}

int p4k_space_protect( p4k_space *s, uintptr_t *base_ptr, size_t *size_ptr, uint32_t protect, uint32_t *old )
{
    uintptr_t base = round_down( *base_ptr, P4K_PAGE_SIZE );
    size_t size = round_up( *base_ptr + *size_ptr, P4K_PAGE_SIZE ) - base;
    struct alloc *a;
    uint8_t first;
    int ret;

    if (!*size_ptr) return P4K_E_INVALID_PARAMETER;
    *old = P4K_PAGE_NOACCESS;
    if (!(a = find_alloc( s, base )) || base + size > a->base + a->size) return P4K_E_INVALID_PARAMETER;

    /* all pages must be committed */
    if (p4k_run_length( get_part, s, base, size, P4K_COMMITTED, &first ) < size || !(first & P4K_COMMITTED))
        return P4K_E_NOT_COMMITTED;
    {
        uint32_t o = p4k_win32_from_vprot( first );
        if ((ret = set_protection( s, a, base, size, protect ))) return ret;
        *old = o;
    }
    *base_ptr = base;
    *size_ptr = size;
    return P4K_OK;
}

int p4k_space_decommit( p4k_space *s, uintptr_t base_in, size_t size_in )
{
    const uintptr_t hmask = s->pol.host_page_size - 1;
    uintptr_t base = round_down( base_in, P4K_PAGE_SIZE );
    size_t size = size_in ? round_up( base_in + size_in, P4K_PAGE_SIZE ) - base : 0;
    struct alloc *a;
    uintptr_t host_start, host_end;
    size_t i, first, n;

    if (!base || !(a = find_alloc( s, base ))) return P4K_E_NOT_ALLOCATED;
    if (a->kind != KIND_PRIVATE) return P4K_E_INVALID_PARAMETER;
    if (!size && base != a->base) return P4K_E_NOT_AT_BASE;
    if (a->base + a->size - base < size) return P4K_E_UNABLE_TO_FREE;
    if (!size) size = a->size;

    host_start = round_up( base, hmask + 1 );
    host_end = round_down( base + size, hmask + 1 );
    if (base + size == a->base + a->size) host_end = a->base + a->host_size;  /* tail of the last host page is ours */

    /* zero the decommitted parts of host pages that stay mapped, then drop whole host pages */
    if (host_start < host_end)
    {
        if (host_start > base && p4k_zero_partial( &s->pol, &s->host.ops, base, host_start - base ))
            return P4K_E_HOST;
        if (host_end < base + size && p4k_zero_partial( &s->pol, &s->host.ops, host_end, base + size - host_end ))
            return P4K_E_HOST;
        if (s->host.discard( s->host.ops.ud, host_start, host_end - host_start )) return P4K_E_HOST;
    }
    else if (p4k_zero_partial( &s->pol, &s->host.ops, base, size )) return P4K_E_HOST;

    first = (base - a->base) >> P4K_PAGE_SHIFT;
    n = size >> P4K_PAGE_SHIFT;
    for (i = 0; i < n; i++) a->pages[first + i] &= ~P4K_COMMITTED;
    return apply( s, base, size );
}

int p4k_space_release( p4k_space *s, uintptr_t base )
{
    struct alloc **pp, *a;

    /* VirtualFree(MEM_RELEASE) for private memory, NtUnmapViewOfSection for an image */
    if (!(a = find_alloc( s, base ))) return P4K_E_NOT_ALLOCATED;
    if (a->base != base) return P4K_E_NOT_AT_BASE;
    if (s->host.unmap( s->host.ops.ud, a->base, a->host_size )) return P4K_E_HOST;
    for (pp = &s->list; *pp != a; pp = &(*pp)->next) ;
    *pp = a->next;
    s->cache = NULL;
    free( a->pages );
    free( a );
    return P4K_OK;
}

int p4k_space_query( p4k_space *s, uintptr_t addr, p4k_mbi *out )
{
    uintptr_t base = round_down( addr, P4K_PAGE_SIZE );
    struct alloc *a, *n;

    if (!in_window( s, base, P4K_PAGE_SIZE )) return P4K_E_INVALID_PARAMETER;
    memset( out, 0, sizeof(*out) );
    out->base = base;
    if (!(a = find_alloc( s, base )))
    {
        uintptr_t end = s->win_base + s->win_size;
        for (n = s->list; n; n = n->next) if (n->base > base) { end = n->base; break; }
        out->region_size = end - base;
        out->state = P4K_MEM_FREE;
        out->protect = P4K_PAGE_NOACCESS;
        return P4K_OK;
    }
    {
        uint8_t first, mask;
        uintptr_t limit = a->base + a->size;
        size_t run;
        uint8_t v = get_part( s, base >> P4K_PAGE_SHIFT );

        /* uncommitted pages form a region regardless of the protection bits they remember */
        mask = (v & P4K_COMMITTED) ? (uint8_t)~P4K_PRIVATE_BITS : P4K_COMMITTED;
        run = p4k_run_length( get_part, s, base, limit - base, mask, &first );
        out->alloc_base = a->base;
        out->alloc_protect = a->alloc_protect;
        out->region_size = run;
        out->state = (first & P4K_COMMITTED) ? P4K_MEM_COMMIT : P4K_MEM_RESERVE;
        out->protect = (first & P4K_COMMITTED) ? p4k_win32_from_vprot( first ) : 0;
        out->type = a->kind == KIND_IMAGE ? P4K_MEM_IMAGE : P4K_MEM_PRIVATE;
    }
    return P4K_OK;
}

int p4k_space_image_begin( p4k_space *s, uintptr_t *base_ptr, size_t image_size )
{
    uintptr_t base = *base_ptr;
    struct alloc *a;
    int ret;

    image_size = round_up( image_size, P4K_PAGE_SIZE );
    if (!image_size) return P4K_E_INVALID_PARAMETER;
    if (base)
    {
        if (base & (GRANULARITY - 1) || !in_window( s, base, image_size )) return P4K_E_INVALID_PARAMETER;
        if (host_overlap( s, base, image_size )) return P4K_E_CONFLICT;
    }
    else if (find_free( s, image_size, &base )) return P4K_E_NO_MEMORY;

    /* pass 1: everything readable and copy-on-write, no X: the loader may write and relocate */
    if ((ret = create_alloc( s, base, image_size, KIND_IMAGE, P4K_PAGE_EXECUTE_WRITECOPY,
                             P4K_COMMITTED | P4K_READ | P4K_WRITECOPY, 0, &a ))) return ret;
    if ((ret = apply( s, base, image_size ))) return ret;
    *base_ptr = base;
    return P4K_OK;
}

int p4k_space_image_finalize( p4k_space *s, uintptr_t base, uint32_t header_size,
                              const p4k_image_section *sec, unsigned nsec )
{
    struct alloc *a = find_alloc( s, base );
    uint32_t hdr = (uint32_t)round_up( header_size, P4K_PAGE_SIZE );
    unsigned i;
    int ret;

    if (!a || a->base != base || a->kind != KIND_IMAGE || hdr > a->size) return P4K_E_INVALID_PARAMETER;
    for (i = 0; i < nsec; i++)
        if ((sec[i].rva & P4K_PAGE_MASK) || (sec[i].size & P4K_PAGE_MASK) || sec[i].rva > a->size ||
            sec[i].size > a->size - sec[i].rva) return P4K_E_INVALID_PARAMETER;

    /* pass 2a: record every final protection in the table, with no host call in between ... */
    if (hdr) set_protection_bits( a, base, hdr, P4K_COMMITTED | P4K_READ );
    for (i = 0; i < nsec; i++)
    {
        uint8_t vprot = P4K_COMMITTED;
        size_t k, first = sec[i].rva >> P4K_PAGE_SHIFT, n = sec[i].size >> P4K_PAGE_SHIFT;

        if (sec[i].characteristics & 0x40000000) vprot |= P4K_READ;
        /* write implies read: keeps Windows-equal protections bit-identical, so 4 KB regions coalesce */
        if (sec[i].characteristics & 0x80000000) vprot |= P4K_WRITECOPY | P4K_READ;
        if (sec[i].characteristics & 0x20000000) vprot |= P4K_EXEC;
        set_protection_bits( a, base + sec[i].rva, sec[i].size, vprot );
        for (k = 0; k < n; k++)
        {
            if (sec[i].native) a->pages[first + k] |= P4K_NATIVE;
            else a->pages[first + k] &= ~P4K_NATIVE;
        }
    }
    /* ... pass 2b: now the host sees only final unions, never an intermediate W|X */
    if (hdr && (ret = apply( s, base, hdr ))) return ret;
    for (i = 0; i < nsec; i++)
        if (sec[i].size && (ret = apply( s, base + sec[i].rva, sec[i].size ))) return ret;
    return P4K_OK;
}

int p4k_space_get_write_watch( p4k_space *s, int reset, uintptr_t base_in, size_t size_in,
                               uintptr_t *addrs, size_t *count )
{
    uintptr_t base = round_down( base_in, P4K_PAGE_SIZE );
    size_t size = round_up( base_in + size_in, P4K_PAGE_SIZE ) - base;
    struct alloc *a;
    size_t pos = 0, cap = *count;
    uintptr_t addr;

    if (!*count || !size) return P4K_E_INVALID_PARAMETER;
    if (!(a = find_alloc( s, base )) || base + size > a->base + a->size || !a->write_watch)
        return P4K_E_INVALID_PARAMETER;

    /* Soft mode: no page ever carries the "clean" bit, so every page is reported as written. */
    for (addr = base; addr < base + size && pos < cap; addr += P4K_PAGE_SIZE)
        if (!(get_part( s, addr >> P4K_PAGE_SHIFT ) & P4K_WRITEWATCH)) addrs[pos++] = addr;
    *count = pos;
    if (reset && !(s->pol.flags & P4K_POLICY_SOFT_WRITEWATCH))
    {
        size_t i, first = (base - a->base) >> P4K_PAGE_SHIFT;
        for (i = 0; i < size >> P4K_PAGE_SHIFT; i++) a->pages[first + i] |= P4K_WRITEWATCH;
        return apply( s, base, size );
    }
    return P4K_OK;
}

int p4k_space_note_write( p4k_space *s, uintptr_t addr )
{
    const uintptr_t hs = s->pol.host_page_size;
    uintptr_t page = round_down( addr, hs ), a_;
    struct alloc *a = find_alloc( s, addr );

    if (!a || !a->write_watch) return P4K_E_INVALID_PARAMETER;
    for (a_ = page; a_ < page + hs; a_ += P4K_PAGE_SIZE)
    {
        struct alloc *b = find_alloc( s, a_ );
        if (b) b->pages[(a_ - b->base) >> P4K_PAGE_SHIFT] &= ~P4K_WRITEWATCH;
    }
    return apply( s, page, hs );
}
