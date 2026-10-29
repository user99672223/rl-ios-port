#!/usr/bin/env python3
"""rlaot: AOT coverage for the game, built on FEX's own code maps and offline compiler (D-007).

FEX records executed guest blocks in "code maps" and FEXOfflineCompiler turns code maps into
relocatable, hash-validated disk-cache DBs. rlaot adds what FEX lacks: static discovery, so
the first device run already has broad coverage, and merging of code maps pulled from the
device.

  rlaot.py scan  --game-dir DIR --out work/aot/manifest.json
  rlaot.py seed  --manifest work/aot/manifest.json --out work/aot/codemaps/seed.codemap
  rlaot.py build --codemaps DIR --out work/aot [--manifest M] [--fex-offline-compiler CMD]
  rlaot.py dump  FILE.codemap

Code map format: FEXCore/include/FEXCore/Core/CodeCache.h (struct CodeMap) and the parser
CodeMap::ParseCodeMap in FEXCore/Source/Interface/Core/CodeCache.cpp. FileId per PE image:
ComputeCodeMapId in Source/Windows/Common/ImageTracker.cpp:
    XXH3_64(lowercase(basename)) ^ ((SizeOfImage << 32) | TimeDateStamp)
Block offsets are RVAs (CodeMapWriter::AppendBlock subtracts FileStartVA, the image base).
Paths are lowercase Windows paths as FEX writes them at runtime (ImageTracker: ToLower(Path)).
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import struct
import subprocess
import sys

import pefile
import xxhash

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recon"))
import recon  # noqa: E402  (shared PE analysis)

# Where the game lives inside Wine on the device (spec 6.3: C:\Games\RocketLeague).
GUEST_GAME_ROOT = "c:\\games\\rocketleague"

# CodeMap markers (CodeCache.h)
LOAD_EXTERNAL_LIBRARY = (0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF)
SET_EXE_MARKER32 = (0xFFFFFFFFFFFFFFFF, 0xFFFFFFEE)
SET_EXE_MARKER64 = (0xFFFFFFFFFFFFFFFF, 0xFFFFFFEF)
ENTRY = struct.Struct("<QI")   # FEX_PACKED { CodeMapFileId FileId; uint32_t BlockOffset; }

MAIN_EXE = "binaries/win64/rocketleague.exe"
# Never loaded by the port (D-005): the EAC bootstrapper and installer, and the .NET launcher.
EXCLUDE = {"binaries/win64/rocketleague_eac.exe", "binaries/win64/launcher.exe",
           "binaries/win64/easyanticheat/easyanticheat_eos_setup.exe"}


def code_map_id(basename, time_date_stamp, size_of_image):
    return xxhash.xxh3_64_intdigest(basename.lower().encode()) ^ ((size_of_image << 32) | time_date_stamp)


def guest_path(rel):
    return GUEST_GAME_ROOT + "\\" + rel.replace("/", "\\").lower()


# ---- scan ---------------------------------------------------------------------------------
def module_seeds(path):
    """Static discovery: entry, exports in code, .pdata starts, TLS callbacks, and relocation
    targets in executable sections that are not inside a .pdata function body."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY[d] for d in (
        "IMAGE_DIRECTORY_ENTRY_EXPORT", "IMAGE_DIRECTORY_ENTRY_EXCEPTION",
        "IMAGE_DIRECTORY_ENTRY_TLS", "IMAGE_DIRECTORY_ENTRY_BASERELOC")])
    oh = pe.OPTIONAL_HEADER
    execs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
             for s in pe.sections if s.Characteristics & recon.IMAGE_SCN_MEM_EXECUTE]

    def in_exec(rva):
        return any(b <= rva < e for b, e in execs)

    seeds = {}

    def add(rva, why):
        if in_exec(rva):
            seeds.setdefault(rva, why)

    if oh.AddressOfEntryPoint:
        add(oh.AddressOfEntryPoint, "entry")
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        exp_dir = oh.DATA_DIRECTORY[0]
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            # Forwarders point inside the export directory, not at code.
            if e.address and not (exp_dir.VirtualAddress <= e.address < exp_dir.VirtualAddress + exp_dir.Size):
                add(e.address, "export")
    ranges = []
    for rf in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", []):
        s = rf.struct
        if s.EndAddress > s.BeginAddress:
            ranges.append((s.BeginAddress, s.EndAddress))
            add(s.BeginAddress, "pdata")
    if hasattr(pe, "DIRECTORY_ENTRY_TLS") and pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks:
        rva = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks - oh.ImageBase
        i = 0
        while True:
            ptr = pe.get_qword_at_rva(rva + 8 * i)
            if not ptr:
                break
            add(ptr - oh.ImageBase, "tls")
            i += 1
    # Relocated 64-bit pointers into code are mostly function pointers (vtables, callbacks).
    # Keep those that start a .pdata function or lie outside every .pdata body (leaf functions).
    ranges.sort()
    starts = {b for b, _ in ranges}
    import bisect
    begins = [b for b, _ in ranges]
    image = pe.get_memory_mapped_image()
    reloc_seeds = 0
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for ent in block.entries:
            if ent.type != pefile.RELOCATION_TYPE["IMAGE_REL_BASED_DIR64"]:
                continue
            ptr = struct.unpack_from("<Q", image, ent.rva)[0] if ent.rva + 8 <= len(image) else 0
            tgt = ptr - oh.ImageBase
            if not (0 <= tgt < oh.SizeOfImage) or not in_exec(tgt):
                continue
            i = bisect.bisect_right(begins, tgt) - 1
            inside = i >= 0 and ranges[i][0] < tgt < ranges[i][1]
            if tgt in starts or not inside:
                if tgt not in seeds:
                    reloc_seeds += 1
                add(tgt, "reloc")
    return {
        "time_date_stamp": pe.FILE_HEADER.TimeDateStamp,
        "size_of_image": oh.SizeOfImage,
        "seeds": sorted(seeds),
        "seed_kinds": {k: sum(1 for v in seeds.values() if v == k) for k in ("entry", "export", "pdata", "tls", "reloc")},
    }


def cmd_scan(game_dir, out):
    root = os.path.realpath(game_dir)
    mods = []
    for dp, _dn, fns in os.walk(root):
        for fn in fns:
            if not fn.lower().endswith((".exe", ".dll")):
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            info = recon.analyze_headers(p, root)
            if info["machine"] != "IMAGE_FILE_MACHINE_AMD64":
                continue
            skip = None
            if rel.lower() in EXCLUDE:
                skip = "never loaded (D-005)"
            elif recon.is_system_dll(info):
                skip = "provided by Wine/DXMT (spec 6.2 skip list)"
            m = {"path": rel, "sha256": info["sha256"], "image_base": info["image_base"],
                 "imports": info["imports"], "delay_imports": info["delay_imports"],
                 "export_count": info["export_count"], "pdata_functions": info["pdata_functions"],
                 "tls_callbacks": info["tls_callbacks"], "relocations": info["relocations"],
                 "sections": info["sections"], "skip": skip}
            if not skip:
                s = module_seeds(p)
                m.update(s)
                m["guest_path"] = guest_path(rel)
                m["file_id"] = f"{code_map_id(os.path.basename(rel), s['time_date_stamp'], s['size_of_image']):016x}"
            mods.append(m)
    mods.sort(key=lambda m: m["path"].lower())
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"guest_game_root": GUEST_GAME_ROOT, "main_exe": MAIN_EXE, "modules": mods},
                              indent=1, sort_keys=True))
    used = [m for m in mods if not m["skip"]]
    print(f"scanned {len(mods)} x86-64 modules; {len(used)} for AOT, "
          f"{sum(len(m['seeds']) for m in used)} seed blocks -> {out}")


# ---- code map read/write ------------------------------------------------------------------
def write_codemap(path, libraries, main_file_id, blocks):
    """libraries: {file_id: guest_path}; blocks: {file_id: iterable of RVAs}.
    Deterministic: libraries and blocks sorted by file id, then offset."""
    buf = bytearray()
    for fid in sorted(libraries):
        buf += ENTRY.pack(*LOAD_EXTERNAL_LIBRARY)
        buf += struct.pack("<Q", fid)
        name = libraries[fid].encode() + b"\0"
        buf += name + b"\0" * ((-len(name)) % 4)          # align to 4 bytes
    buf += ENTRY.pack(*SET_EXE_MARKER64) + struct.pack("<Q", main_file_id)
    for fid in sorted(blocks):
        for off in sorted(set(blocks[fid])):
            buf += ENTRY.pack(fid, off)
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(path).write_bytes(bytes(buf))


def parse_codemap(data):
    """Python mirror of CodeMap::ParseCodeMap. Returns {file_id: {filename, blocks, bitness}}."""
    ret = {}
    i = 0
    while i + ENTRY.size <= len(data):
        fid, off = ENTRY.unpack_from(data, i)
        i += ENTRY.size
        if (fid, off) == LOAD_EXTERNAL_LIBRARY:
            if i + 8 > len(data):
                break
            ext = struct.unpack_from("<Q", data, i)[0]
            i += 8
            end = data.index(b"\0", i)
            name = data[i:end].decode()
            i = end + 1
            i += (-(len(name) + 1)) % 4
            ret.setdefault(ext, {"filename": "", "blocks": set(), "bitness": None})["filename"] = name
        elif (fid, off) in (SET_EXE_MARKER32, SET_EXE_MARKER64):
            if i + 8 > len(data):
                break
            exe = struct.unpack_from("<Q", data, i)[0]
            i += 8
            ret.setdefault(exe, {"filename": "", "blocks": set(), "bitness": None})["bitness"] = \
                32 if (fid, off) == SET_EXE_MARKER32 else 64
        else:
            if fid not in ret:
                if fid == 0xFFFFFFFFFFFFFFFF:
                    raise ValueError("malformed code map")
                print(f"warning: code map references unknown file id {fid:016x}", file=sys.stderr)
            else:
                ret[fid]["blocks"].add(off)
    return ret


def cmd_seed(manifest, out):
    m = json.loads(pathlib.Path(manifest).read_text())
    used = [x for x in m["modules"] if not x["skip"]]
    libs = {int(x["file_id"], 16): x["guest_path"] for x in used}
    blocks = {int(x["file_id"], 16): x["seeds"] for x in used}
    main = [x for x in used if x["path"].lower() == m["main_exe"]]
    if not main:
        sys.exit(f"main executable {m['main_exe']} not in manifest")
    write_codemap(out, libs, int(main[0]["file_id"], 16), blocks)
    print(f"seed code map: {len(libs)} modules, {sum(len(b) for b in blocks.values())} blocks -> {out}")


# ---- build --------------------------------------------------------------------------------
def cmd_build(codemaps, out, manifest=None, fex_offline_compiler=None):
    out = pathlib.Path(out)
    files = sorted(pathlib.Path(codemaps).glob("**/*")) if pathlib.Path(codemaps).is_dir() else [pathlib.Path(codemaps)]
    files = [f for f in files if f.is_file()]
    seed = out / "codemaps/seed.codemap"
    if seed.exists() and seed not in files:
        files.append(seed)
    if not files:
        sys.exit(f"no code maps in {codemaps}")
    merged = {}
    main_ids = set()
    for f in files:
        try:
            parsed = parse_codemap(f.read_bytes())
        except (ValueError, UnicodeDecodeError) as e:
            print(f"skip {f}: {e}", file=sys.stderr)
            continue
        for fid, c in parsed.items():
            e = merged.setdefault(fid, {"filename": c["filename"], "blocks": set(), "bitness": None})
            e["filename"] = e["filename"] or c["filename"]
            e["blocks"] |= c["blocks"]
            if c["bitness"]:
                e["bitness"] = c["bitness"]
                main_ids.add(fid)
    if len(main_ids) != 1:
        sys.exit(f"expected exactly one main executable across code maps, found {len(main_ids)}")
    known = None
    if manifest:
        man = json.loads(pathlib.Path(manifest).read_text())
        known = {int(x["file_id"], 16) for x in man["modules"] if not x["skip"]}
    stale = [fid for fid in merged if known is not None and fid not in known]
    for fid in stale:
        # Different TimeDateStamp/SizeOfImage: the game updated, or a Wine builtin module.
        print(f"note: file id {fid:016x} ({merged[fid]['filename']}) is not a current game module; kept as-is")
    main = main_ids.pop()
    ready = out / "codemaps/merged.codemap"
    write_codemap(ready, {f: e["filename"] for f, e in merged.items()}, main,
                  {f: e["blocks"] for f, e in merged.items()})
    stats = {e["filename"] or f"{f:016x}": len(e["blocks"]) for f, e in sorted(merged.items())}
    (out / "codemaps/merged.json").write_text(json.dumps(stats, indent=1, sort_keys=True))
    total = sum(stats.values())
    print(f"merged {len(files)} code maps: {len(merged)} modules, {total} blocks -> {ready}")
    sha = hashlib.sha256(ready.read_bytes()).hexdigest()
    print(f"merged code map sha256 {sha}")

    cmd = fex_offline_compiler or os.environ.get("RLAOT_FEX_OFFLINE_COMPILER")
    if not cmd:
        print("cache generation skipped: no FEXOfflineCompiler configured (RISKS.md R-004). "
              "Set RLAOT_FEX_OFFLINE_COMPILER or pass --fex-offline-compiler.")
        return 0
    cache = out / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    full = shlex.split(cmd) + ["generate", "--outdir", str(cache), str(ready)]
    print("running:", " ".join(shlex.quote(c) for c in full))
    return subprocess.run(full).returncode


def cmd_dump(path):
    for fid, c in sorted(parse_codemap(pathlib.Path(path).read_bytes()).items()):
        print(f"{fid:016x} {c['bitness'] or '-':>2} {len(c['blocks']):7d} blocks  {c['filename']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("scan")
    p.add_argument("--game-dir", required=True)
    p.add_argument("--out", default="work/aot/manifest.json")
    p = sp.add_parser("seed")
    p.add_argument("--manifest", default="work/aot/manifest.json")
    p.add_argument("--out", default="work/aot/codemaps/seed.codemap")
    p = sp.add_parser("build")
    p.add_argument("--codemaps", required=True)
    p.add_argument("--out", default="work/aot")
    p.add_argument("--manifest", default=None)
    p.add_argument("--fex-offline-compiler", default=None)
    p = sp.add_parser("dump")
    p.add_argument("file")
    a = ap.parse_args()
    if a.cmd == "scan":
        return cmd_scan(a.game_dir, a.out)
    if a.cmd == "seed":
        return cmd_seed(a.manifest, a.out)
    if a.cmd == "build":
        return cmd_build(a.codemaps, a.out, a.manifest, a.fex_offline_compiler)
    if a.cmd == "dump":
        return cmd_dump(a.file)


if __name__ == "__main__":
    sys.exit(main())
