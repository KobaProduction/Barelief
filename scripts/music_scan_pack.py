#!/usr/bin/env python3
"""ZIP64 wrapper for completed Barelief music-scan collections.

Each existing .brscan is already compressed and remains byte-for-byte intact.
Streaming ZIP_STORED packaging uses bounded RAM; native .brscan files have
a separate <1 GiB and <=50k-entry loading limit, so a 20 GiB *ordinary*
.brscan is NOT an option without a new native archive protocol.

Commands:
    pack SOURCE_DIRECTORY TARGET.brscanpack
    inspect TARGET.brscanpack
    verify TARGET.brscanpack
    extract TARGET.brscanpack PART_NUMBER OUTPUT.brscan
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path

COPY_BUFFER = 2 * 1024 * 1024
FORMAT = "barelief.music_scan_pack"
PART_PREFIX = "parts/"
RAW_KIND = "barelief.music_scan_collection"


def manifest_parts(source: Path) -> tuple[dict, list[tuple[Path, dict]]]:
    meta = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    if meta.get("kind") != RAW_KIND or meta.get("version") != 1:
        raise ValueError("Not a completed Barelief corpus scan")
    listed = meta.get("parts")
    if not isinstance(listed, list) or not listed:
        raise ValueError("Empty or missing scan parts")
    actual = {p.name for p in source.glob("music-scan-part-*.brscan")}
    expected = {item["name"] for item in listed}
    if actual != expected:
        raise ValueError(f"Missing/unlisted parts: {len(expected-actual)} / {len(actual-expected)}")
    requested = measured = failed = 0
    files = []
    for number, item in enumerate(listed, 1):
        name = item.get("name")
        if name != f"music-scan-part-{number:06}.brscan":
            raise ValueError(f"Wrong part sequence: {name}")
        if any(item.get(k, -1) < 0 for k in ("requested_count", "processed_count", "failed_count")):
            raise ValueError(f"Invalid counters: {name}")
        if item["requested_count"] != item["processed_count"] + item["failed_count"]:
            raise ValueError(f"Part counters inconsistent: {name}")
        file = source / name
        if not file.is_file() or file.is_symlink():
            raise ValueError(f"Not a regular archive file: {name}")
        with zipfile.ZipFile(file) as inner:
            data = json.loads(inner.read("manifest.json"))["document"]
        stats = data.get("analysis_run") or {}
        if any(stats.get(key) != item.get(key) for key in
               ("requested_count", "processed_count", "failed_count")):
            raise ValueError(f"Measurements disagree with outer manifest: {name}")
        if len(data.get("compositions", [])) != item["processed_count"]:
            raise ValueError(f"Composition count inconsistent: {name}")
        requested += item["requested_count"]
        measured += item["processed_count"]
        failed += item["failed_count"]
        files.append((file, item))
    if (requested, measured, failed) != tuple(meta.get(k) for k in
        ("requested_count", "processed_count", "failed_count")):
        raise ValueError("Global measurement counters mismatch")
    return meta, files


def copy_and_hash(source, destination) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    while data := source.read(COPY_BUFFER):
        destination.write(data)
        digest.update(data)
        size += len(data)
    return size, digest.hexdigest()


def pack(folder: Path, target: Path) -> None:
    if target.exists() or target.with_name(target.name + ".partial").exists():
        raise FileExistsError("Target or incomplete target already exists")
    meta, files = manifest_parts(folder)
    target.parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(target.parent).free
    required = sum(p.stat().st_size for p, _ in files) + 64 * 1024 * 1024
    if free < required:
        raise OSError(f"Not enough free disk space: {free} bytes vs {required}")
    partial = target.with_name(target.name + ".partial")
    started = time.monotonic()
    index = []
    try:
        with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_STORED,
                             allowZip64=True, strict_timestamps=False) as destination:
            destination.writestr("manifest.json", json.dumps(
                meta, ensure_ascii=False, separators=(",", ":")).encode())
            for n, (part, stats) in enumerate(files, 1):
                entry = PART_PREFIX + part.name
                # file metadata is safe, but archive paths are explicitly fixed
                with part.open("rb") as src, destination.open(entry, "w", force_zip64=True) as out:
                    actual_size, sha = copy_and_hash(src, out)
                if actual_size != part.stat().st_size:
                    raise OSError(f"Source changed while reading: {part.name}")
                index.append({"name": part.name, "size": actual_size, "sha256": sha,
                              "requested_count": stats["requested_count"],
                              "processed_count": stats["processed_count"],
                              "failed_count": stats["failed_count"]})
                if n % 100 == 0 or n == len(files):
                    print(f"PACKED {n}/{len(files)} parts, "
                          f"{sum(p['size'] for p in index)/2**30:.2f} GiB",
                          flush=True)
            document = {"kind": FORMAT, "version": 1,
                        "reference": "manifest.json", "parts": index,
                        "storage": "uncompressed ZIP64 of compressed Barelief .brscan members",
                        "completed": True}
            destination.writestr("pack-index.json", json.dumps(
                document, ensure_ascii=False, separators=(",", ":")).encode())
        os.replace(partial, target)
    except BaseException:
        # No deletion of source corpus. Remove only our own partial output.
        partial.unlink(missing_ok=True)
        raise
    print(f"COMPLETE path={target} bytes={target.stat().st_size} "
          f"time={time.monotonic()-started:.1f}s", flush=True)


def read_pack_index(z: zipfile.ZipFile) -> tuple[dict, dict]:
    idx = json.loads(z.read("pack-index.json"))
    main = json.loads(z.read("manifest.json"))
    if idx.get("kind") != FORMAT or idx.get("version") != 1 or not idx.get("completed"):
        raise ValueError("Not a completed Barelief scan-pack")
    if main.get("kind") != RAW_KIND:
        raise ValueError("Wrong scan manifest")
    listed = idx.get("parts", [])
    if len(listed) != len(main.get("parts", [])):
        raise ValueError("Different numbers of parts")
    expected = {"manifest.json", "pack-index.json"} | {
        PART_PREFIX + p["name"] for p in listed
    }
    if set(z.namelist()) != expected:
        raise ValueError("Unexpected or missing outer ZIP members")
    for part, summary in zip(listed, main["parts"]):
        if any(part.get(k) != summary.get(k) for k in
               ("name", "requested_count", "processed_count", "failed_count")):
            raise ValueError("Part and manifest counters differ")
    return idx, main


def verify(target: Path) -> None:
    started = time.monotonic()
    with zipfile.ZipFile(target, allowZip64=True) as z:
        index, meta = read_pack_index(z)
        for n, item in enumerate(index["parts"], 1):
            name = PART_PREFIX + item["name"]
            info = z.getinfo(name)
            if info.file_size != item["size"] or info.compress_type != zipfile.ZIP_STORED:
                raise ValueError(f"ZIP member metadata mismatch: {name}")
            with z.open(info) as src:
                actual_size, sha = copy_and_hash(src, _NullWriter())
            if actual_size != item["size"] or sha != item["sha256"]:
                raise ValueError(f"SHA-256 mismatch: {name}")
            if n % 100 == 0 or n == len(index["parts"]):
                print(f"VERIFIED {n}/{len(index['parts'])} parts", flush=True)
        print(f"VERIFIED_PACK bytes={target.stat().st_size} "
              f"parts={len(index['parts'])} requested={meta['requested_count']} "
              f"processed={meta['processed_count']} failed={meta['failed_count']} "
              f"elapsed={time.monotonic()-started:.1f}s", flush=True)


class _NullWriter:
    def write(self, data): pass


def inspect(target: Path) -> None:
    with zipfile.ZipFile(target, allowZip64=True) as z:
        index, meta = read_pack_index(z)
        print(json.dumps({"bytes": target.stat().st_size,
                          "parts": len(index["parts"]),
                          "requested": meta["requested_count"],
                          "processed": meta["processed_count"],
                          "failed": meta["failed_count"],
                          "random_access": True,
                          "reader": "ZIP64, read each parts/*.brscan in sequence"},
                         ensure_ascii=False, indent=2))


def extract(target: Path, number: int, output: Path) -> None:
    if output.exists(): raise FileExistsError(str(output))
    with zipfile.ZipFile(target, allowZip64=True) as z:
        index, _ = read_pack_index(z)
        if not 1 <= number <= len(index["parts"]):
            raise ValueError("Part index out of range")
        entry = index["parts"][number - 1]
        tmp = output.with_name(output.name + ".partial")
        if tmp.exists(): raise FileExistsError(str(tmp))
        try:
            with z.open(PART_PREFIX + entry["name"]) as src, tmp.open("wb") as dst:
                size, digest = copy_and_hash(src, dst)
            if size != entry["size"] or digest != entry["sha256"]:
                raise ValueError("Inner ZIP hash mismatch")
            os.replace(tmp, output)
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
        print(f"EXTRACTED {number} -> {output} bytes={size}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    a = sub.add_parser("pack"); a.add_argument("source", type=Path); a.add_argument("target", type=Path)
    a = sub.add_parser("verify"); a.add_argument("target", type=Path)
    a = sub.add_parser("inspect"); a.add_argument("target", type=Path)
    a = sub.add_parser("extract"); a.add_argument("target", type=Path)
    a.add_argument("number", type=int); a.add_argument("output", type=Path)
    arg = parser.parse_args()
    if arg.command == "pack": pack(arg.source, arg.target)
    elif arg.command == "verify": verify(arg.target)
    elif arg.command == "inspect": inspect(arg.target)
    elif arg.command == "extract": extract(arg.target, arg.number, arg.output)


if __name__ == "__main__":
    main()
