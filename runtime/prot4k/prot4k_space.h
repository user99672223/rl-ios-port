/*
 * prot4k_space - self-contained address-space model built on prot4k.h.
 * Used by the tests and by embedders that do not have Wine's views.  Not mirrored into Wine.
 */
#ifndef PROT4K_SPACE_H
#define PROT4K_SPACE_H

#include "prot4k.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================================
 * Self-contained address-space object (used by the tests and by embedders that do not have
 * Wine's views).  Models VirtualAlloc/VirtualProtect/VirtualFree/VirtualQuery/GetWriteWatch
 * over a window of addresses on top of the primitives above.
 * ==================================================================================== */

typedef struct p4k_space p4k_space;

/* Status codes (the NTSTATUS Wine would return is noted). */
enum
{
    P4K_OK = 0,
    P4K_E_INVALID_PARAMETER,        /* STATUS_INVALID_PARAMETER                    */
    P4K_E_INVALID_PAGE_PROTECTION,  /* STATUS_INVALID_PAGE_PROTECTION              */
    P4K_E_NOT_COMMITTED,            /* STATUS_NOT_COMMITTED                        */
    P4K_E_CONFLICT,                 /* STATUS_CONFLICTING_ADDRESSES                */
    P4K_E_NOT_MAPPED,               /* STATUS_NOT_MAPPED_VIEW                      */
    P4K_E_NOT_ALLOCATED,            /* STATUS_MEMORY_NOT_ALLOCATED                 */
    P4K_E_NO_MEMORY,                /* STATUS_NO_MEMORY                            */
    P4K_E_NOT_AT_BASE,              /* STATUS_FREE_VM_NOT_AT_BASE                  */
    P4K_E_UNABLE_TO_FREE,           /* STATUS_UNABLE_TO_FREE_VM                    */
    P4K_E_HOST                      /* host mprotect failed (STATUS_ACCESS_DENIED) */
};

typedef struct p4k_space_host
{
    p4k_host_ops ops;
    /* reserve host address space: new zero-filled pages, host prot NONE */
    int (*map)( void *ud, uintptr_t addr, size_t len );
    int (*unmap)( void *ud, uintptr_t addr, size_t len );
    /* replace with fresh zero-filled pages, host prot NONE (decommit of whole host pages) */
    int (*discard)( void *ud, uintptr_t addr, size_t len );
} p4k_space_host;

typedef struct p4k_mbi
{
    uintptr_t base;             /* BaseAddress  */
    uintptr_t alloc_base;       /* AllocationBase (0 if free) */
    uint32_t  alloc_protect;    /* AllocationProtect */
    size_t    region_size;      /* RegionSize */
    uint32_t  state;            /* MEM_COMMIT / MEM_RESERVE / MEM_FREE */
    uint32_t  protect;          /* Protect */
    uint32_t  type;             /* MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE (0 if free) */
} p4k_mbi;

typedef struct p4k_image_section
{
    uint32_t rva;               /* 4 KB aligned */
    uint32_t size;              /* multiple of 4 KB */
    uint32_t characteristics;   /* IMAGE_SCN_MEM_READ 0x40000000 / WRITE 0x80000000 / EXECUTE 0x20000000 */
    int      native;            /* section holds native host code (P4K_NATIVE) */
} p4k_image_section;

/* The space covers [win_base, win_base + win_size); allocations are placed on 64 KB
 * boundaries.  'host' is copied. Returns NULL on error. */
p4k_space *p4k_space_new( const p4k_policy *p, const p4k_space_host *host,
                          uintptr_t win_base, size_t win_size );
void p4k_space_free( p4k_space *s );

/* VirtualAlloc / NtAllocateVirtualMemory.  *base is in/out (0 = anywhere), *size in/out.
 * 'type' = MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH. */
int p4k_space_alloc( p4k_space *s, uintptr_t *base, size_t *size, uint32_t type, uint32_t protect );

/* VirtualProtect / NtProtectVirtualMemory. */
int p4k_space_protect( p4k_space *s, uintptr_t *base, size_t *size, uint32_t protect, uint32_t *old );

/* VirtualFree(MEM_DECOMMIT) (size 0 = whole allocation, base must then be the allocation base)
 * and VirtualFree(MEM_RELEASE) / NtUnmapViewOfSection (base must be the allocation base). */
int p4k_space_decommit( p4k_space *s, uintptr_t base, size_t size );
int p4k_space_release( p4k_space *s, uintptr_t base );

/* VirtualQuery. */
int p4k_space_query( p4k_space *s, uintptr_t addr, p4k_mbi *out );

/* Two-pass PE image mapping.  begin(): reserves+commits the image pages as R|WC (host RW, no X)
 * so the loader can copy sections and apply relocations.  finalize(): records the final section
 * protections in the table first, then applies them to the host (never a transient W|X). */
int p4k_space_image_begin( p4k_space *s, uintptr_t *base, size_t image_size );
int p4k_space_image_finalize( p4k_space *s, uintptr_t base, uint32_t header_size,
                              const p4k_image_section *sec, unsigned nsec );

/* GetWriteWatch / ResetWriteWatch.  Without faults the layer cannot learn which pages were
 * written: every page of a watched range is conservatively reported as written (and reset is a
 * no-op).  Returns the page addresses.  *count in: capacity, out: number written. */
int p4k_space_get_write_watch( p4k_space *s, int reset, uintptr_t base, size_t size,
                               uintptr_t *addrs, size_t *count );

/* Fault-mode write watch only (policy without P4K_POLICY_SOFT_WRITEWATCH): what Wine's
 * virtual_handle_fault() does when a write hits a watched page: the whole host page loses its
 * "clean" bits (a host fault cannot tell the 4 KB parts apart) and the host protection is
 * restored. Not used in soft mode. */
int p4k_space_note_write( p4k_space *s, uintptr_t addr );

/* Introspection for tests. */
uint8_t p4k_space_part( p4k_space *s, uintptr_t addr );   /* part byte, 0 if unknown */


#ifdef __cplusplus
}
#endif
#endif /* PROT4K_SPACE_H */
