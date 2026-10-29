/*
 * prot4k - 4 KB Windows page protections over larger host pages (16 KB on Apple silicon).
 *
 * Core logic only: no Wine types, no libc beyond <stdint.h>/<stddef.h>/<string.h>.
 * The same file is mirrored byte-for-byte into Wine (dlls/ntdll/unix/prot4k.h) by
 * patches/wine/0100-*.patch; tests/prot4k checks that the mirror has not drifted.
 *
 * Model
 * -----
 *  - Windows sees 4 KB pages.  Each 4 KB page has one protection byte ("part byte").
 *    The bit layout is identical to Wine's VPROT_* flags (dlls/ntdll/unix/virtual.c:134-140)
 *    so Wine's pages_vprot[] table can be handed to this code unchanged.
 *  - The host sees host_page_size pages (a power of two, 4 KB..64 KB).  Every host page
 *    covers host_page_size/4096 parts.  The host protection of a host page is the most
 *    permissive union of the contributions of its parts (p4k_host_prot()).
 *  - W^X: the host protection never has W and X together when P4K_POLICY_ENFORCE_WX is set.
 *    Guest (emulated) code is never executed natively, so with P4K_POLICY_DROP_GUEST_X the
 *    X bit of a part contributes to the host only if the part is flagged P4K_NATIVE.
 *  - PAGE_GUARD / write watch: with P4K_POLICY_SOFT_GUARD / P4K_POLICY_SOFT_WRITEWATCH they are
 *    pure bookkeeping (reported by queries, never armed on the host, so no faults).  Without them
 *    the layer behaves like stock Wine: a guarded part makes the whole host page inaccessible and a
 *    watched part makes it read-only, and the fault handler then fixes things up per host page.
 *  - With an empty policy the result is bit-for-bit what stock Wine computes
 *    (tests/prot4k/test_unit.c: test_equals_stock_wine).
 *
 * All address arguments are plain uintptr_t so the code can be tested on any host.
 */
#ifndef PROT4K_H
#define PROT4K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P4K_PAGE_SHIFT 12
#define P4K_PAGE_SIZE  ((uintptr_t)1 << P4K_PAGE_SHIFT)
#define P4K_PAGE_MASK  (P4K_PAGE_SIZE - 1)
#define P4K_MAX_PARTS  16           /* host page up to 64 KB */

/* ---- per-4KB-page protection byte (same bits as Wine's VPROT_*) ----------------------- */
#define P4K_READ        0x01
#define P4K_WRITE       0x02
#define P4K_EXEC        0x04
#define P4K_WRITECOPY   0x08
#define P4K_GUARD       0x10
#define P4K_COMMITTED   0x20
#define P4K_WRITEWATCH  0x40   /* set = watched and still clean (Wine's meaning) */
#define P4K_NATIVE      0x80   /* part holds native host code: X must reach the host   */
/* bits that are invisible to Windows callers (must be masked out when coalescing queries) */
#define P4K_PRIVATE_BITS (P4K_WRITEWATCH | P4K_NATIVE)

/* ---- host protections (numerically equal to PROT_READ/PROT_WRITE/PROT_EXEC) ---------- */
#define P4K_HOST_NONE   0
#define P4K_HOST_READ   1
#define P4K_HOST_WRITE  2
#define P4K_HOST_EXEC   4

/* ---- Win32 constants (numerically equal to winnt.h) ---------------------------------- */
#define P4K_PAGE_NOACCESS          0x01
#define P4K_PAGE_READONLY          0x02
#define P4K_PAGE_READWRITE         0x04
#define P4K_PAGE_WRITECOPY         0x08
#define P4K_PAGE_EXECUTE           0x10
#define P4K_PAGE_EXECUTE_READ      0x20
#define P4K_PAGE_EXECUTE_READWRITE 0x40
#define P4K_PAGE_EXECUTE_WRITECOPY 0x80
#define P4K_PAGE_GUARD             0x100
#define P4K_MEM_COMMIT             0x1000
#define P4K_MEM_RESERVE            0x2000
#define P4K_MEM_FREE               0x10000
#define P4K_MEM_PRIVATE            0x20000
#define P4K_MEM_MAPPED             0x40000
#define P4K_MEM_WRITE_WATCH        0x00200000
#define P4K_MEM_IMAGE              0x1000000

/* ---- policy -------------------------------------------------------------------------- */
#define P4K_POLICY_DROP_GUEST_X    0x01 /* X reaches the host only for P4K_NATIVE parts   */
#define P4K_POLICY_ENFORCE_WX      0x02 /* never emit W|X; X is dropped on conflict        */
#define P4K_POLICY_SOFT_GUARD      0x04 /* a PAGE_GUARD part keeps its access on the host  */
#define P4K_POLICY_SOFT_WRITEWATCH 0x08 /* write-watch never removes host W (no faults)    */

typedef struct p4k_policy
{
    uintptr_t host_page_size;   /* power of two, 4096..65536 */
    unsigned  flags;            /* P4K_POLICY_* */
} p4k_policy;

/* Returns 0 if the policy is usable. */
int p4k_policy_check( const p4k_policy *p );

/* Parts per host page. */
static inline unsigned p4k_parts( const p4k_policy *p ) { return (unsigned)(p->host_page_size >> P4K_PAGE_SHIFT); }

/* ---- Win32 <-> part byte conversion (mirrors get_vprot_flags / get_win32_prot) -------- */

/* Returns 0 on success, -1 for an invalid protection value (STATUS_INVALID_PAGE_PROTECTION).
 * 'image' selects the SEC_IMAGE rule: PAGE_READWRITE becomes copy-on-write. The result does
 * not include P4K_COMMITTED. */
int p4k_vprot_from_win32( uint32_t protect, int image, uint8_t *vprot );

/* Win32 protection reported to callers for a committed part (includes PAGE_GUARD). */
uint32_t p4k_win32_from_vprot( uint8_t vprot );

/* ---- union computation --------------------------------------------------------------- */

/* Host contribution of a single part (P4K_HOST_*). */
unsigned p4k_part_prot( uint8_t part, unsigned policy_flags );

/* Most permissive union of 'n' parts after applying (part & ~clear) | set to every part.
 * Sets *conflict to 1 when W and X both survived and X had to be dropped (ENFORCE_WX). */
unsigned p4k_host_prot( const p4k_policy *p, const uint8_t *parts, unsigned n,
                        uint8_t set, uint8_t clear, int *conflict );

/* ---- host protection application ------------------------------------------------------ */

/* Table accessor: returns the part byte of 4 KB page number 'page' (= address >> 12).
 * Pages the table does not know about must read as 0. */
typedef uint8_t (*p4k_get_part_fn)( void *ud, uintptr_t page );

/* Host side effects. Every address/length passed in is aligned to host_page_size. */
typedef struct p4k_host_ops
{
    int (*mprotect)( void *ud, uintptr_t addr, size_t len, int prot ); /* 0 or -1  */
    int (*zero)( void *ud, uintptr_t addr, size_t len );               /* 4 KB aligned, host page is RW */
    void *ud;
    /* optional: a native-code part shares the host page with a writable part, so X was dropped
     * to keep the page W^X (native code in that page can no longer be executed) */
    void (*conflict)( void *ud, uintptr_t host_page_addr );
} p4k_host_ops;

/* Apply the union protections to every host page overlapping [addr, addr+size), after
 * applying set/clear to the parts.  Adjacent host pages with equal protection are merged
 * into one mprotect call.  Returns 0, or -1 at the first mprotect failure.
 * Reads the parts through 'get'. */
int p4k_apply_range( const p4k_policy *p, p4k_get_part_fn get, void *get_ud,
                     const p4k_host_ops *host, uintptr_t addr, size_t size,
                     uint8_t set, uint8_t clear );

/* Zero the 4 KB pages in [addr, addr+size) that live in host pages which will *stay* mapped
 * (the partial head/tail of a decommit).  The caller has already updated the table (the
 * COMMITTED bit is cleared) but not yet called p4k_apply_range(): this temporarily makes each
 * affected host page RW (never X), zeroes, and leaves the final protection to the next
 * p4k_apply_range().  'zero_host_aligned_only' pages that are fully covered are skipped,
 * because the caller discards them (fresh zero pages).  Returns 0 or -1. */
int p4k_zero_partial( const p4k_policy *p, const p4k_host_ops *host, uintptr_t addr, size_t size );

/* ---- query helpers -------------------------------------------------------------------- */

/* Length in bytes of the run, starting at 'addr' (4 KB aligned), of pages whose part byte
 * equals the first page's under 'mask', limited to 'size' bytes.  *first receives the first
 * page's byte (all bits, not masked).  Mirrors get_vprot_range_size() (virtual.c:1153).
 * Pass mask = ~P4K_PRIVATE_BITS to get Windows-visible regions. */
size_t p4k_run_length( p4k_get_part_fn get, void *get_ud, uintptr_t addr, size_t size,
                       uint8_t mask, uint8_t *first );

#ifdef __cplusplus
}
#endif
#endif /* PROT4K_H */
