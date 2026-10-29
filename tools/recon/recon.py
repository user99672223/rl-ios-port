#!/usr/bin/env python3
"""Static recon over a Windows game install (spec section 7, step 2).

Reads GAME_DIR only; never writes there. Produces:
  work/recon/recon.json        full machine-readable results (game-derived, gitignored)
  work/RECON.md                full human-readable report (game-derived, gitignored)
  docs/RECON_SUMMARY.md        sanitized: names, counts, and booleans only; no hashes,
                               addresses, or bytes from the game

Usage: recon.py --game-dir DIR [--work DIR] [--summary FILE] [--jobs N]
"""
import argparse
import collections
import concurrent.futures as cf
import hashlib
import json
import os
import re
import sys

import capstone
import pefile

# --- Windows constants (winnt.h) -------------------------------------------------------
PAGE_EXECUTE_ANY = 0x10 | 0x20 | 0x40 | 0x80   # EXECUTE, _READ, _READWRITE, _WRITECOPY
PAGE_GUARD = 0x100
MEM_WRITE_WATCH = 0x200000
KUSER_SHARED_DATA = 0x7FFE0000
MSVC_SET_THREAD_NAME_EXCEPTION = 0x406D1388

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

# Imports worth calling out, grouped by the question they answer.
API_GROUPS = {
    "exec_memory": {"VirtualProtect", "VirtualProtectEx", "VirtualAlloc", "VirtualAllocEx",
                    "VirtualAlloc2", "NtProtectVirtualMemory", "NtAllocateVirtualMemory",
                    "FlushInstructionCache", "CreateFileMappingW", "MapViewOfFile",
                    "MapViewOfFileEx"},
    "page_granularity": {"GetWriteWatch", "ResetWriteWatch", "VirtualQuery", "VirtualQueryEx",
                         "GetSystemInfo", "GetNativeSystemInfo", "VirtualLock"},
    "thread_naming": {"SetThreadDescription", "GetThreadDescription", "RaiseException"},
    "process_spawn": {"CreateProcessW", "CreateProcessA", "ShellExecuteW", "ShellExecuteA",
                      "ShellExecuteExW", "WinExec", "CreateProcessAsUserW"},
    "dynamic_load": {"LoadLibraryW", "LoadLibraryA", "LoadLibraryExW", "LoadLibraryExA",
                     "GetProcAddress", "LdrLoadDll"},
    "exceptions": {"AddVectoredExceptionHandler", "SetUnhandledExceptionFilter",
                   "RtlAddFunctionTable", "RtlInstallFunctionTableCallback",
                   "AddVectoredContinueHandler"},
    "fibers": {"ConvertThreadToFiber", "CreateFiber", "SwitchToFiber", "CreateFiberEx"},
    "timing": {"QueryPerformanceCounter", "QueryPerformanceFrequency", "timeBeginPeriod",
               "GetTickCount64", "Sleep", "SwitchToThread"},
}
CRT_DLL_RE = re.compile(r"^(ucrtbase|vcruntime\d+(_\d)?|msvcp\d+(_\w+)?|api-ms-win-crt-.*|msvcr\d+)\.dll$", re.I)

# Module categories by DLL name (imported or found as a string).
CATEGORY_RE = {
    "graphics": r"^(d3d\d+|dxgi|d3dcompiler_\d+|d3d9|opengl32|vulkan-1|nvapi64|amd_ags_x64)\.dll$",
    "audio": r"^(xaudio2_\d+(redist)?|x3daudio1_\d|xapofx1_\d|dsound|winmm|mmdevapi|wwise.*|fmod.*)\.dll$",
    "input": r"^(xinput1_\d|xinput9_1_0|dinput8|hid|setupapi|gameinput)\.dll$",
    "network": r"^(ws2_32|wsock32|winhttp|wininet|iphlpapi|dnsapi|secur32|crypt32|bcrypt|ncrypt|libcurl.*)\.dll$",
    "platform_sdk": r"^(eossdk-win64-shipping|discord_game_sdk|steam_api64|galaxy64)\.dll$",
    "anticheat": r"(easyanticheat|eac|battleye)",
}

ADDR_RE = re.compile(r"\[(0x[0-9a-f]+)\]")
GS_RE = re.compile(r"gs:\[(0x[0-9a-f]+)\]")
RIP_RE = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+)\]")
DLL_STR_RE = re.compile(rb"([A-Za-z0-9_\-\.]{2,64}\.(?:dll|DLL|Dll))\x00")
DLL_WSTR_RE = re.compile(rb"((?:[A-Za-z0-9_\-\.]\x00){2,64}\.\x00[dD]\x00[lL]\x00[lL]\x00)\x00\x00")
EXE_STR_RE = re.compile(rb"([A-Za-z0-9_\-\.]{2,64}\.(?:exe|EXE))\x00")
EXE_WSTR_RE = re.compile(rb"((?:[A-Za-z0-9_\-\.]\x00){2,64}\.\x00[eE]\x00[xX]\x00[eE]\x00)\x00\x00")
# Command-line switches we care about (e.g. -noeac); matched as ASCII and UTF-16LE.
SWITCHES = [b"noeac", b"nosteam", b"EpicPortal", b"AUTH_PASSWORD", b"AUTH_TYPE", b"epicapp",
            b"epicenv", b"epicusername", b"epicuserid", b"epiclocale", b"epicsandboxid"]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sec_perms(c):
    return ("R" if c & IMAGE_SCN_MEM_READ else "-") + \
           ("W" if c & IMAGE_SCN_MEM_WRITE else "-") + \
           ("X" if c & IMAGE_SCN_MEM_EXECUTE else "-")


def strings_of(data, ascii_re, wide_re):
    out = set()
    for m in ascii_re.finditer(data):
        out.add(m.group(1).decode("ascii").lower())
    for m in wide_re.finditer(data):
        out.add(m.group(1).decode("utf-16le").lower())
    return out


def categorize(names):
    cats = collections.defaultdict(set)
    for n in names:
        for cat, rx in CATEGORY_RE.items():
            if re.search(rx, n, re.I):
                cats[cat].add(n)
    return {k: sorted(v) for k, v in cats.items()}


def last_imm(window, reg_names):
    """Find the most recent `mov <reg>, imm` for any of reg_names in a short instruction window."""
    for (_a, _s, mn, ops) in reversed(window):
        if mn in ("mov", "xor") and ops.split(",")[0].strip() in reg_names:
            if mn == "xor":
                parts = [p.strip() for p in ops.split(",")]
                if len(parts) == 2 and parts[0] == parts[1]:
                    return 0
                return None
            src = ops.split(",", 1)[1].strip()
            try:
                return int(src, 0)
            except ValueError:
                return None
        # Any other write to the register ends the search.
        if ops.split(",")[0].strip() in reg_names:
            return None
    return None


R8 = {"r8", "r8d"}
R9 = {"r9", "r9d"}


def analyze_code(path):
    """Disassemble every .pdata function and collect instruction-level facts."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
    ])
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()

    iat = {}  # IAT slot RVA -> "dll!func"
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = entry.dll.decode(errors="replace").lower()
        for imp in entry.imports:
            name = imp.name.decode(errors="replace") if imp.name else f"#{imp.ordinal}"
            iat[imp.address - base] = f"{dll}!{name}"

    funcs = []
    for rf in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", []):
        s = rf.struct
        if s.EndAddress > s.BeginAddress:
            funcs.append((s.BeginAddress, s.EndAddress))
    funcs = sorted(set(funcs))

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    c = collections.Counter()
    kuser_sites, fixed_addr_sites = collections.Counter(), collections.Counter()
    gs_offsets = collections.Counter()
    vp_calls = collections.Counter()  # (api, protect/flags) -> count
    seh_name_const = 0
    insn_total = 0

    for begin, end in funcs:
        code = image[begin:end]
        window = []
        reg_from_iat = {}
        for (addr, size, mn, ops) in md.disasm_lite(code, begin):
            insn_total += 1
            if mn == "cpuid":
                c["cpuid"] += 1
            elif mn == "xgetbv":
                c["xgetbv"] += 1
            elif mn in ("rdtsc", "rdtscp"):
                c["rdtsc"] += 1
            elif mn in ("int3",):
                pass
            if mn.startswith("v") and mn not in ("verr", "verw"):
                if "zmm" in ops:
                    c["avx512_insn"] += 1
                elif "ymm" in ops:
                    c["avx256_insn"] += 1
                else:
                    c["vex128_insn"] += 1
            if "gs:[" in ops:
                m = GS_RE.search(ops)
                gs_offsets[m.group(1) if m else "reg"] += 1
            if "[0x" in ops and "rip" not in ops and "gs:" not in ops and "fs:" not in ops:
                m = ADDR_RE.search(ops)
                if m:
                    a = int(m.group(1), 16)
                    if KUSER_SHARED_DATA <= a < KUSER_SHARED_DATA + 0x1000:
                        kuser_sites[hex(a)] += 1
                    elif a >= 0x10000:
                        fixed_addr_sites[hex(a)] += 1
            if hex(MSVC_SET_THREAD_NAME_EXCEPTION) in ops:
                seh_name_const += 1

            # Resolve calls through the IAT: `call [rip+X]`, or `mov reg,[rip+X]; call reg`.
            target_api = None
            if "rip" in ops:
                m = RIP_RE.search(ops)
                if m:
                    disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                    slot = addr + size + disp
                    if slot in iat:
                        if mn in ("call", "jmp"):
                            target_api = iat[slot]
                        elif mn == "mov":
                            reg_from_iat[ops.split(",")[0].strip()] = iat[slot]
            if mn in ("call", "jmp") and target_api is None and ops in reg_from_iat:
                target_api = reg_from_iat[ops]
            if target_api:
                fn = target_api.split("!", 1)[1]
                if fn in ("VirtualProtect",):
                    p = last_imm(window, R8)
                    vp_calls[(fn, "dynamic" if p is None else hex(p))] += 1
                elif fn in ("VirtualAlloc",):
                    t, p = last_imm(window, R8), last_imm(window, R9)
                    vp_calls[(fn, ("dynamic" if t is None else hex(t)) + "/" +
                              ("dynamic" if p is None else hex(p)))] += 1
                elif fn in ("VirtualProtectEx", "VirtualAllocEx", "VirtualAlloc2"):
                    vp_calls[(fn, "see-args")] += 1
            window.append((addr, size, mn, ops))
            if len(window) > 12:
                window.pop(0)

    exec_protect = sum(n for (fn, k), n in vp_calls.items() if fn == "VirtualProtect"
                       and k != "dynamic" and int(k, 16) & PAGE_EXECUTE_ANY)
    guard_protect = sum(n for (fn, k), n in vp_calls.items() if fn == "VirtualProtect"
                        and k != "dynamic" and int(k, 16) & PAGE_GUARD)
    exec_alloc = guard_alloc = write_watch_alloc = 0
    for (fn, k), n in vp_calls.items():
        if fn != "VirtualAlloc":
            continue
        t, p = k.split("/")
        if p != "dynamic" and int(p, 16) & PAGE_EXECUTE_ANY:
            exec_alloc += n
        if p != "dynamic" and int(p, 16) & PAGE_GUARD:
            guard_alloc += n
        if t != "dynamic" and int(t, 16) & MEM_WRITE_WATCH:
            write_watch_alloc += n

    return {
        "functions_disassembled": len(funcs),
        "instructions": insn_total,
        "counts": dict(c),
        "kuser_shared_data_reads": dict(kuser_sites),
        "other_fixed_address_refs": len(fixed_addr_sites),
        "gs_offsets": dict(gs_offsets.most_common(12)),
        "msvc_thread_name_exception_const": seh_name_const,
        "memory_api_calls": {f"{fn}({k})": n for (fn, k), n in sorted(vp_calls.items())},
        "exec_virtualprotect_const": exec_protect,
        "guard_virtualprotect_const": guard_protect,
        "exec_virtualalloc_const": exec_alloc,
        "guard_virtualalloc_const": guard_alloc,
        "write_watch_virtualalloc_const": write_watch_alloc,
    }


def analyze_headers(path, root):
    rel = os.path.relpath(path, root)
    with open(path, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY[d] for d in (
            "IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_DIRECTORY_ENTRY_EXPORT",
            "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT", "IMAGE_DIRECTORY_ENTRY_TLS",
            "IMAGE_DIRECTORY_ENTRY_EXCEPTION", "IMAGE_DIRECTORY_ENTRY_BASERELOC",
            "IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG")])
    machine = pefile.MACHINE_TYPE.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))
    oh = pe.OPTIONAL_HEADER
    imports = collections.defaultdict(list)
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = entry.dll.decode(errors="replace").lower()
        for imp in entry.imports:
            imports[dll].append(imp.name.decode(errors="replace") if imp.name else f"#{imp.ordinal}")
    delay = collections.defaultdict(list)
    for entry in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        dll = entry.dll.decode(errors="replace").lower()
        for imp in entry.imports:
            delay[dll].append(imp.name.decode(errors="replace") if imp.name else f"#{imp.ordinal}")
    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        exports = [e.name.decode(errors="replace") if e.name else f"#{e.ordinal}"
                   for e in pe.DIRECTORY_ENTRY_EXPORT.symbols]
    tls_callbacks = 0
    if hasattr(pe, "DIRECTORY_ENTRY_TLS") and pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks:
        rva = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks - oh.ImageBase
        while True:
            ptr = pe.get_qword_at_rva(rva + 8 * tls_callbacks)
            if not ptr:
                break
            tls_callbacks += 1
    relocs = sum(len(b.entries) for b in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []))
    sections = [{"name": s.Name.rstrip(b"\0").decode(errors="replace"),
                 "rva": s.VirtualAddress, "vsize": s.Misc_VirtualSize,
                 "perms": sec_perms(s.Characteristics)} for s in pe.sections]
    # A 16 KB host page can mix 4 KB parts with different protections when sections are not
    # 16 KB aligned. Count section boundaries that fall inside a 16 KB page.
    sub16k_boundaries = sum(1 for s in pe.sections[1:] if s.VirtualAddress % 0x4000)
    all_imported = set(imports) | set(delay)
    dll_strings = strings_of(data, DLL_STR_RE, DLL_WSTR_RE) - {os.path.basename(path).lower()}
    exe_strings = strings_of(data, EXE_STR_RE, EXE_WSTR_RE) - {os.path.basename(path).lower()}
    switches = {s.decode(): bool(re.search(re.escape(s), data, re.I) or
                                 re.search(re.escape(s.decode().encode("utf-16le")), data, re.I))
                for s in SWITCHES}
    flat_imports = {fn for fns in list(imports.values()) + list(delay.values()) for fn in fns}
    api_hits = {g: sorted(flat_imports & names) for g, names in API_GROUPS.items()}
    # APIs resolved by name at runtime show up as strings rather than imports.
    api_strings = {g: sorted(n for n in names - flat_imports if n.encode() + b"\0" in data)
                   for g, names in API_GROUPS.items()}
    lc = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    cfg = bool(lc and getattr(lc.struct, "GuardCFFunctionTable", 0))
    return {
        "path": rel,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "machine": machine,
        "is_dll": bool(pe.FILE_HEADER.Characteristics & 0x2000),
        "subsystem": oh.Subsystem,
        "image_base": hex(oh.ImageBase),
        "size_of_image": oh.SizeOfImage,
        "section_alignment": oh.SectionAlignment,
        "dynamic_base": bool(oh.DllCharacteristics & 0x40),
        "high_entropy_va": bool(oh.DllCharacteristics & 0x20),
        "cfg": cfg,
        "entry_rva": hex(oh.AddressOfEntryPoint),
        "sections": sections,
        "wx_sections": [s["name"] for s in sections if "W" in s["perms"] and "X" in s["perms"]],
        "sub16k_section_boundaries": sub16k_boundaries,
        "imports": {k: sorted(v) for k, v in sorted(imports.items())},
        "delay_imports": {k: sorted(v) for k, v in sorted(delay.items())},
        "export_count": len(exports),
        "pdata_functions": len(getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", [])),
        "tls_callbacks": tls_callbacks,
        "relocations": relocs,
        "crt": {"imported": sorted(d for d in all_imported if CRT_DLL_RE.match(d))},
        "runtime_dll_strings": sorted(dll_strings - all_imported),
        "exe_strings": sorted(exe_strings),
        "switch_strings": switches,
        "api_imports": api_hits,
        "api_by_name_strings": api_strings,
        "categories": categorize(all_imported | dll_strings),
    }


def scan_one(args):
    path, root, deep = args
    try:
        info = analyze_headers(path, root)
    except pefile.PEFormatError as e:
        return {"path": os.path.relpath(path, root), "error": str(e)}
    if deep and info["machine"] == "IMAGE_FILE_MACHINE_AMD64":
        info["code"] = analyze_code(path)
    return info


def data_breakdown(root):
    by_dir, by_ext = collections.Counter(), collections.Counter()
    total = files = 0
    for dp, _dn, fns in os.walk(root):
        top = os.path.relpath(dp, root).split(os.sep)[0]
        for fn in fns:
            sz = os.path.getsize(os.path.join(dp, fn))
            total += sz
            files += 1
            by_dir[top] += sz
            by_ext[os.path.splitext(fn)[1].lower() or "(none)"] += sz
    return {"total_bytes": total, "files": files, "by_top_dir": dict(by_dir.most_common()),
            "by_extension": dict(by_ext.most_common(15))}


def gib(n):
    return f"{n / (1 << 30):.2f} GiB"


def mib(n):
    return f"{n / (1 << 20):.1f} MiB"


def is_system_dll(m):
    """Redistributable Microsoft DLLs that Wine provides as builtins (skipped by rlaot)."""
    n = os.path.basename(m["path"]).lower()
    return bool(re.match(r"^(api-ms-win-.*|ucrtbase|vcruntime\d+.*|msvcp\d+.*|concrt\d+|vccorlib\d+|"
                         r"vcomp\d+|dbghelp|d3dcompiler_\d+|xaudio2_\d+.*|x3daudio1_\d|xapofx1_\d|"
                         r"xinput1_\d|d3d\d+|dxgi)\.dll$", n))


def write_reports(res, work, summary_path):
    mods = [m for m in res["modules"] if "error" not in m]
    game_mods = [m for m in mods if not is_system_dll(m)]
    L = []
    L.append("# RECON (full, game-derived — do not commit)\n")
    L.append(f"Game dir: `{res['game_dir']}`  \nData: {gib(res['data']['total_bytes'])} in "
             f"{res['data']['files']} files\n")
    L.append("## Modules\n")
    L.append("| module | machine | size | sha256 | pref. base | dyn | pdata | TLS cb | relocs | W+X | sub-16K bounds |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for m in mods:
        L.append(f"| {m['path']} | {m['machine'].replace('IMAGE_FILE_MACHINE_', '')} | {mib(m['size'])} | "
                 f"`{m['sha256'][:16]}…` | {m['image_base']} | {'y' if m['dynamic_base'] else 'n'} | "
                 f"{m['pdata_functions']} | {m['tls_callbacks']} | {m['relocations']} | "
                 f"{','.join(m['wx_sections']) or '-'} | {m['sub16k_section_boundaries']} |")
    for m in game_mods:
        L.append(f"\n## {m['path']}\n")
        L.append(f"- sha256 `{m['sha256']}`, base {m['image_base']}, SizeOfImage {mib(m['size_of_image'])}, "
                 f"entry {m['entry_rva']}, exports {m['export_count']}, CFG {m['cfg']}")
        L.append("- sections: " + ", ".join(f"{s['name']} {s['perms']} {mib(s['vsize'])}" for s in m["sections"]))
        L.append("- imports: " + ", ".join(f"{d} ({len(f)})" for d, f in m["imports"].items()))
        if m["delay_imports"]:
            L.append("- delay imports: " + ", ".join(f"{d} ({len(f)})" for d, f in m["delay_imports"].items()))
        L.append("- CRT imported: " + (", ".join(m["crt"]["imported"]) or "none (static CRT)"))
        L.append("- DLL names in strings, not imported: " + (", ".join(m["runtime_dll_strings"]) or "none"))
        L.append("- EXE names in strings: " + (", ".join(m["exe_strings"]) or "none"))
        L.append("- switch strings present: " + ", ".join(k for k, v in m["switch_strings"].items() if v))
        for g in API_GROUPS:
            if m["api_imports"][g] or m["api_by_name_strings"][g]:
                L.append(f"- {g}: imported {m['api_imports'][g]}; by-name strings {m['api_by_name_strings'][g]}")
        if "code" in m:
            cd = m["code"]
            L.append(f"- code: {cd['functions_disassembled']} functions, {cd['instructions']} instructions; "
                     f"counts {cd['counts']}")
            L.append(f"- KUSER_SHARED_DATA reads: {cd['kuser_shared_data_reads'] or 'none'}; "
                     f"other absolute refs: {cd['other_fixed_address_refs']}")
            L.append(f"- gs: offsets {cd['gs_offsets']}")
            L.append(f"- MSVC thread-name exception constant sites: {cd['msvc_thread_name_exception_const']}")
            L.append(f"- memory API call sites (const args): {cd['memory_api_calls'] or 'none'}")
    (work / "RECON.md").write_text("\n".join(L) + "\n")

    # ---- sanitized summary: names, counts, booleans; no hashes, addresses, or bytes ----
    def anyc(key):
        return sum(m.get("code", {}).get(key, 0) for m in game_mods)

    def anycount(key):
        return sum(m.get("code", {}).get("counts", {}).get(key, 0) for m in game_mods)

    S = []
    S.append("# Recon summary (sanitized)\n")
    S.append("Generated by `tools/recon/recon.py`. Contains only module names, counts and yes/no "
             "facts about the install, with no hashes, addresses or game bytes. The full report "
             "is `work/RECON.md` and never leaves the laptop.\n")
    S.append(f"- Install size: {gib(res['data']['total_bytes'])} across {res['data']['files']} files")
    S.append("- Top-level dirs: " + ", ".join(f"{k} {gib(v)}" for k, v in res["data"]["by_top_dir"].items()))
    S.append("- Largest file types: " + ", ".join(f"{k} {gib(v)}" for k, v in list(res["data"]["by_extension"].items())[:8]))
    S.append(f"- PE modules: {len(mods)} total, {len(game_mods)} game/third-party, "
             f"{len(mods) - len(game_mods)} Microsoft redistributables (Wine provides these)")
    machines = collections.Counter(m["machine"].replace("IMAGE_FILE_MACHINE_", "") for m in mods)
    S.append(f"- Machines: {dict(machines)}\n")
    S.append("## Game and third-party modules\n")
    S.append("| module | size | .pdata functions | TLS callbacks | dynamic base | W+X sections | CRT |")
    S.append("|---|---|---|---|---|---|---|")
    for m in game_mods:
        S.append(f"| {m['path']} | {mib(m['size'])} | {m['pdata_functions']} | {m['tls_callbacks']} | "
                 f"{'yes' if m['dynamic_base'] else 'no'} | {', '.join(m['wx_sections']) or 'none'} | "
                 f"{'imported' if m['crt']['imported'] else 'static'} |")
    S.append("\n## Findings that drive the design\n")
    for m in game_mods:
        if "code" not in m:
            continue
        cd = m["code"]
        name = os.path.basename(m["path"])
        S.append(f"### {name}")
        S.append(f"- Imported DLLs: {', '.join(m['imports'])}")
        if m["delay_imports"]:
            S.append(f"- Delay-loaded DLLs: {', '.join(m['delay_imports'])}")
        if m["runtime_dll_strings"]:
            S.append(f"- DLL names referenced as strings (candidate runtime loads): {', '.join(m['runtime_dll_strings'])}")
        if m["exe_strings"]:
            S.append(f"- EXE names referenced as strings: {', '.join(m['exe_strings'])}")
        sw = [k for k, v in m["switch_strings"].items() if v]
        if sw:
            S.append(f"- Command-line switch strings present: {', '.join(sw)}")
        for g in ("exec_memory", "page_granularity", "thread_naming", "process_spawn", "exceptions", "fibers"):
            hits = m["api_imports"][g] + [f"{x} (by name)" for x in m["api_by_name_strings"][g]]
            if hits:
                S.append(f"- {g.replace('_', ' ')} APIs: {', '.join(hits)}")
        c = cd["counts"]
        S.append(f"- Code: {cd['functions_disassembled']} functions disassembled; cpuid sites {c.get('cpuid', 0)}, "
                 f"xgetbv {c.get('xgetbv', 0)}, rdtsc {c.get('rdtsc', 0)}, AVX-256 instructions {c.get('avx256_insn', 0)}, "
                 f"AVX-512 {c.get('avx512_insn', 0)}, VEX-128 {c.get('vex128_insn', 0)}")
        S.append(f"- Fixed-address reads: KUSER_SHARED_DATA {sum(cd['kuser_shared_data_reads'].values())} sites; "
                 f"gs: segment accesses {sum(cd['gs_offsets'].values())}")
        S.append(f"- Thread naming: SetThreadDescription "
                 f"{'yes' if 'SetThreadDescription' in m['api_imports']['thread_naming'] + m['api_by_name_strings']['thread_naming'] else 'no'}, "
                 f"MSVC exception constant sites {cd['msvc_thread_name_exception_const']}")
        S.append(f"- VirtualProtect/VirtualAlloc with constant executable protection: "
                 f"{cd['exec_virtualprotect_const'] + cd['exec_virtualalloc_const']} sites; "
                 f"PAGE_GUARD {cd['guard_virtualprotect_const'] + cd['guard_virtualalloc_const']}; "
                 f"MEM_WRITE_WATCH {cd['write_watch_virtualalloc_const']}; "
                 f"call sites with non-constant arguments: "
                 f"{sum(n for k, n in cd['memory_api_calls'].items() if 'dynamic' in k or 'see-args' in k)}")
        S.append("")
    S.append("## Totals across game modules\n")
    S.append(f"- AVX-256 instructions: {anycount('avx256_insn')}; AVX-512: {anycount('avx512_insn')}; cpuid sites: {anycount('cpuid')}")
    S.append(f"- Constant PAGE_GUARD uses: {anyc('guard_virtualprotect_const') + anyc('guard_virtualalloc_const')}; "
             f"MEM_WRITE_WATCH: {anyc('write_watch_virtualalloc_const')}")
    S.append(f"- Constant executable VirtualProtect/VirtualAlloc: {anyc('exec_virtualprotect_const') + anyc('exec_virtualalloc_const')}")
    cats = collections.defaultdict(set)
    for m in mods:
        for k, v in m["categories"].items():
            cats[k] |= set(v)
    S.append("\n## Subsystem DLLs seen (imports or strings)\n")
    for k in ("graphics", "audio", "input", "network", "platform_sdk", "anticheat"):
        S.append(f"- {k.replace('_', ' ')}: {', '.join(sorted(cats.get(k, []))) or 'none'}")
    summary_path.write_text("\n".join(S) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--work", default="work")
    ap.add_argument("--summary", default="docs/RECON_SUMMARY.md")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    import pathlib
    root = os.path.realpath(a.game_dir)
    work = pathlib.Path(a.work)
    (work / "recon").mkdir(parents=True, exist_ok=True)
    pes = []
    for dp, _dn, fns in os.walk(root):
        for fn in fns:
            if fn.lower().endswith((".exe", ".dll")):
                pes.append(os.path.join(dp, fn))
    pes.sort()
    jobs = []
    for p in pes:
        name = os.path.basename(p).lower()
        deep = not re.match(r"^api-ms-win-", name)
        jobs.append((p, root, deep))
    # Biggest first so the long disassembly overlaps with the rest.
    jobs.sort(key=lambda j: -os.path.getsize(j[0]))
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        modules = list(ex.map(scan_one, jobs))
    modules.sort(key=lambda m: m["path"].lower())
    res = {"game_dir": root, "data": data_breakdown(root), "modules": modules}
    (work / "recon" / "recon.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    write_reports(res, work, pathlib.Path(a.summary))
    print(f"scanned {len(modules)} PE files; wrote {work}/RECON.md, {work}/recon/recon.json, {a.summary}")


if __name__ == "__main__":
    sys.exit(main())
