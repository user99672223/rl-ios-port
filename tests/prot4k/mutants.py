"""Deliberate breaks of the prot4k library, applied to throw-away copies by negative_check.sh.

Usage: mutants.py list | mutants.py apply NAME DIR
Each mutant is (file, old text, new text).  The old text must occur exactly as written so a
refactor of the library cannot silently turn a mutant into a no-op.
"""
import sys

MUTANTS = {
    # --- union logic -------------------------------------------------------------------
    "union-first-part-only": ("prot4k.c",
        "    for (i = 0; i < n; i++)\n    {\n        uint8_t part",
        "    for (i = 0; i < 1 && i < n; i++)\n    {\n        uint8_t part"),
    "union-and-instead-of-or": ("prot4k.c",
        "        prot |= p4k_part_prot( part, p->flags );",
        "        prot = i ? (prot & p4k_part_prot( part, p->flags )) : p4k_part_prot( part, p->flags );"),
    # --- W^X rules ---------------------------------------------------------------------
    "wx-not-enforced": ("prot4k.c",
        "if ((p->flags & P4K_POLICY_ENFORCE_WX) &&", "if (0 &&"),
    "guest-x-not-dropped": ("prot4k.c",
        "if (!(policy_flags & P4K_POLICY_DROP_GUEST_X) || (part & P4K_NATIVE))", "if (1)"),
    "native-bit-ignored": ("prot4k.c",
        "if (!(policy_flags & P4K_POLICY_DROP_GUEST_X) || (part & P4K_NATIVE))",
        "if (!(policy_flags & P4K_POLICY_DROP_GUEST_X))"),
    # --- guard / write watch -----------------------------------------------------------
    "guard-always-hard": ("prot4k.c",
        "if ((part & P4K_GUARD) && !(policy_flags & P4K_POLICY_SOFT_GUARD)) return 0;",
        "if ((part & P4K_GUARD)) return 0;"),
    "guard-not-armed": ("prot4k.c",
        "if (armed_guard) prot = 0;", "if (armed_guard && 0) prot = 0;"),
    "soft-guard-still-armed": ("prot4k.c",
        "if ((part & P4K_GUARD) && !(p->flags & P4K_POLICY_SOFT_GUARD)) armed_guard = 1;",
        "if ((part & P4K_GUARD)) armed_guard = 1;"),
    "watch-not-armed": ("prot4k.c",
        "else if (armed_watch)", "else if (armed_watch && 0)"),
    "soft-watch-still-armed": ("prot4k.c",
        "if ((part & P4K_WRITEWATCH) && !(p->flags & P4K_POLICY_SOFT_WRITEWATCH)) armed_watch = 1;",
        "if ((part & P4K_WRITEWATCH)) armed_watch = 1;"),
    # --- host call sequence ------------------------------------------------------------
    "range-too-short": ("prot4k.c",
        "return host->mprotect( host->ud, run_start, end - run_start, (int)run_prot );",
        "return host->mprotect( host->ud, run_start, end - run_start -\n"
        "                          (end - run_start > p->host_page_size ? p->host_page_size : 0), (int)run_prot );"),
    "runs-never-split": ("prot4k.c",
        "if (have_run && prot == run_prot) continue;", "if (have_run) continue;"),
    # --- decommit zero fill ------------------------------------------------------------
    "no-zero-on-partial-decommit": ("prot4k.c",
        "if (host->zero( host->ud, lo, hi - lo )) return -1;", "(void)lo; (void)hi;"),
    # --- query coalescing / bookkeeping -----------------------------------------------
    "query-splits-on-hidden-bits": ("prot4k_space.c",
        "mask = (v & P4K_COMMITTED) ? (uint8_t)~P4K_PRIVATE_BITS : P4K_COMMITTED;", "mask = (uint8_t)(0xff | (v & 0));"),
    "protect-drops-native-bit": ("prot4k_space.c",
        "vprot | (a->pages[first + i] & P4K_PRIVATE_BITS);", "vprot;"),
    "old-protection-wrong": ("prot4k_space.c",
        "uint32_t o = p4k_win32_from_vprot( first );", "uint32_t o = p4k_win32_from_vprot( first & ~P4K_GUARD );"),
}

if __name__ == "__main__":
    if sys.argv[1] == "list":
        print("\n".join(MUTANTS))
    else:
        name, d = sys.argv[2], sys.argv[3]
        f, old, new = MUTANTS[name]
        path = d + "/" + f
        src = open(path).read()
        if src.count(old) < 1:
            print("MUTATION PATTERN NOT FOUND:", name, repr(old)); sys.exit(3)
        open(path, "w").write(src.replace(old, new, 1))
