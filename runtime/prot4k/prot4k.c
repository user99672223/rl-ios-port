/*
 * prot4k - core logic. See prot4k.h.  C11, freestanding apart from <string.h>.
 * Mirrored byte-for-byte into Wine by patches/wine/0100-*.patch.
 */

#if 0
#pragma makedep unix
#endif

#include <string.h>
#include "prot4k.h"

int p4k_policy_check( const p4k_policy *p )
{
    uintptr_t s = p->host_page_size;
    if (s < P4K_PAGE_SIZE || s > P4K_PAGE_SIZE * P4K_MAX_PARTS) return -1;
    if (s & (s - 1)) return -1;
    return 0;
}

/* ---- Win32 conversion --------------------------------------------------------------------- */

int p4k_vprot_from_win32( uint32_t protect, int image, uint8_t *vprot )
{
    uint8_t v;

    switch (protect & 0xff)
    {
    case P4K_PAGE_READONLY:          v = P4K_READ; break;
    case P4K_PAGE_READWRITE:         v = image ? (P4K_READ | P4K_WRITECOPY) : (P4K_READ | P4K_WRITE); break;
    case P4K_PAGE_WRITECOPY:         v = P4K_READ | P4K_WRITECOPY; break;
    case P4K_PAGE_EXECUTE:           v = P4K_EXEC; break;
    case P4K_PAGE_EXECUTE_READ:      v = P4K_EXEC | P4K_READ; break;
    case P4K_PAGE_EXECUTE_READWRITE: v = image ? (P4K_EXEC | P4K_READ | P4K_WRITECOPY)
                                               : (P4K_EXEC | P4K_READ | P4K_WRITE); break;
    case P4K_PAGE_EXECUTE_WRITECOPY: v = P4K_EXEC | P4K_READ | P4K_WRITECOPY; break;
    case P4K_PAGE_NOACCESS:          v = 0; break;
    default: return -1;
    }
    if (protect & P4K_PAGE_GUARD) v |= P4K_GUARD;
    *vprot = v;
    return 0;
}

uint32_t p4k_win32_from_vprot( uint8_t vprot )
{
    static const uint8_t flags[16] =
    {
        P4K_PAGE_NOACCESS,          /* 0 */
        P4K_PAGE_READONLY,          /* READ */
        P4K_PAGE_READWRITE,         /* WRITE */
        P4K_PAGE_READWRITE,         /* READ | WRITE */
        P4K_PAGE_EXECUTE,           /* EXEC */
        P4K_PAGE_EXECUTE_READ,      /* READ | EXEC */
        P4K_PAGE_EXECUTE_READWRITE, /* WRITE | EXEC */
        P4K_PAGE_EXECUTE_READWRITE, /* READ | WRITE | EXEC */
        P4K_PAGE_WRITECOPY,         /* WRITECOPY */
        P4K_PAGE_WRITECOPY,         /* READ | WRITECOPY */
        P4K_PAGE_WRITECOPY,         /* WRITE | WRITECOPY */
        P4K_PAGE_WRITECOPY,         /* READ | WRITE | WRITECOPY */
        P4K_PAGE_EXECUTE_WRITECOPY, /* EXEC | WRITECOPY */
        P4K_PAGE_EXECUTE_WRITECOPY, /* READ | EXEC | WRITECOPY */
        P4K_PAGE_EXECUTE_WRITECOPY, /* WRITE | EXEC | WRITECOPY */
        P4K_PAGE_EXECUTE_WRITECOPY  /* READ | WRITE | EXEC | WRITECOPY */
    };
    uint32_t ret = flags[vprot & 0x0f];
    if (vprot & P4K_GUARD) ret |= P4K_PAGE_GUARD;
    return ret;
}

/* ---- union ----------------------------------------------------------------------------------- */

unsigned p4k_part_prot( uint8_t part, unsigned policy_flags )
{
    unsigned prot = 0;

    /* A decommitted part contributes nothing, whatever protection bits it remembers. */
    if (!(part & P4K_COMMITTED)) return 0;
    /* Guard and write-watch are "armed" states handled at host-page level by p4k_host_prot();
     * here a guarded part only contributes its ordinary access when the guard is soft. */
    if ((part & P4K_GUARD) && !(policy_flags & P4K_POLICY_SOFT_GUARD)) return 0;

    if (part & P4K_READ) prot |= P4K_HOST_READ;
    if (part & (P4K_WRITE | P4K_WRITECOPY)) prot |= P4K_HOST_WRITE | P4K_HOST_READ;
    if (part & P4K_EXEC)
    {
        /* Execute-only and execute-read both need to be readable: the emulator fetches the
         * guest bytes with ordinary loads. */
        prot |= P4K_HOST_READ;
        if (!(policy_flags & P4K_POLICY_DROP_GUEST_X) || (part & P4K_NATIVE)) prot |= P4K_HOST_EXEC;
    }
    return prot;
}

unsigned p4k_host_prot( const p4k_policy *p, const uint8_t *parts, unsigned n,
                        uint8_t set, uint8_t clear, int *conflict )
{
    unsigned i, prot = 0;
    int armed_guard = 0, armed_watch = 0;

    for (i = 0; i < n; i++)
    {
        uint8_t part = (uint8_t)((parts[i] & ~clear) | set);

        prot |= p4k_part_prot( part, p->flags );
        if (!(part & P4K_COMMITTED)) continue;
        if ((part & P4K_GUARD) && !(p->flags & P4K_POLICY_SOFT_GUARD)) armed_guard = 1;
        if ((part & P4K_WRITEWATCH) && !(p->flags & P4K_POLICY_SOFT_WRITEWATCH)) armed_watch = 1;
    }
    /* Fault-armed mode (same as stock Wine): the host cannot arm one 4 KB part, so a guard arms
     * the whole host page (the first access to any part faults and the guard fires, which
     * over-reports for the neighbours) and a watched part makes the whole page read-only (a
     * write to any part faults and every part of the host page is then reported as written).
     * In soft mode neither is ever armed: the union above is the whole story. */
    if (armed_guard) prot = 0;
    else if (armed_watch) prot &= ~(unsigned)P4K_HOST_WRITE;

    if (conflict) *conflict = 0;
    if ((p->flags & P4K_POLICY_ENFORCE_WX) && (prot & P4K_HOST_WRITE) && (prot & P4K_HOST_EXEC))
    {
        prot &= ~(unsigned)P4K_HOST_EXEC;
        if (conflict) *conflict = 1;
    }
    return prot;
}

/* ---- host application -------------------------------------------------------------------------- */

static void load_parts( const p4k_policy *p, p4k_get_part_fn get, void *ud, uintptr_t host_addr,
                        uint8_t *parts )
{
    unsigned i, n = p4k_parts( p );
    uintptr_t page = host_addr >> P4K_PAGE_SHIFT;

    for (i = 0; i < n; i++) parts[i] = get( ud, page + i );
}

int p4k_apply_range( const p4k_policy *p, p4k_get_part_fn get, void *get_ud,
                     const p4k_host_ops *host, uintptr_t addr, size_t size,
                     uint8_t set, uint8_t clear )
{
    const uintptr_t hmask = p->host_page_size - 1;
    uintptr_t cur = addr & ~hmask;
    uintptr_t end = (addr + size + hmask) & ~hmask;
    uintptr_t run_start = cur;
    unsigned run_prot = 0;
    uint8_t parts[P4K_MAX_PARTS];
    int have_run = 0;

    if (end <= cur) return 0;
    for (; cur < end; cur += p->host_page_size)
    {
        unsigned prot;
        int conflict;
        load_parts( p, get, get_ud, cur, parts );
        prot = p4k_host_prot( p, parts, p4k_parts( p ), set, clear, &conflict );
        if (conflict && host->conflict) host->conflict( host->ud, cur );
        if (have_run && prot == run_prot) continue;
        if (have_run && host->mprotect( host->ud, run_start, cur - run_start, (int)run_prot )) return -1;
        run_start = cur;
        run_prot = prot;
        have_run = 1;
    }
    return host->mprotect( host->ud, run_start, end - run_start, (int)run_prot );
}

int p4k_zero_partial( const p4k_policy *p, const p4k_host_ops *host, uintptr_t addr, size_t size )
{
    const uintptr_t hmask = p->host_page_size - 1;
    uintptr_t range_end = addr + size;
    uintptr_t cur = addr & ~hmask;
    uintptr_t end = (range_end + hmask) & ~hmask;

    for (; cur < end; cur += p->host_page_size)
    {
        uintptr_t lo = cur < addr ? addr : cur;
        uintptr_t hi = cur + p->host_page_size < range_end ? cur + p->host_page_size : range_end;

        if (lo == cur && hi == cur + p->host_page_size) continue;  /* fully covered: discarded */
        if (host->mprotect( host->ud, cur, p->host_page_size, P4K_HOST_READ | P4K_HOST_WRITE )) return -1;
        if (host->zero( host->ud, lo, hi - lo )) return -1;
    }
    return 0;
}

/* ---- query -------------------------------------------------------------------------------------- */

size_t p4k_run_length( p4k_get_part_fn get, void *get_ud, uintptr_t addr, size_t size,
                       uint8_t mask, uint8_t *first )
{
    uintptr_t page = addr >> P4K_PAGE_SHIFT;
    size_t i, n = size >> P4K_PAGE_SHIFT;
    uint8_t v = get( get_ud, page );

    *first = v;
    for (i = 1; i < n; i++)
        if ((get( get_ud, page + i ) ^ v) & mask) return i << P4K_PAGE_SHIFT;
    return size;
}
