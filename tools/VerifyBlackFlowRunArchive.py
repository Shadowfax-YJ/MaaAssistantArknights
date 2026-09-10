"""Verify an automatically generated BlackFlow ZIP without extracting it.

Exit 0: valid LOCAL signature and content; 2: unsigned/old/manually repacked;
1: invalid or damaged. A local signature does not attest an official client.
Requires cryptography (pip install cryptography).
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile

DOMAIN = b"MAA-BLACKFLOW-ARCHIVE-v1\n"
MAX_TOTAL_BYTES = 16 * 1024**3
MAX_INDEX_BYTES = 8 * 1024**2


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON field: " + key)
        result[key] = value
    return result


def parse(data):
    return json.loads(data, object_pairs_hook=unique_object)


def require(value, message):
    if not value:
        raise ValueError(message)


def safe_name(name):
    path = PurePosixPath(name)
    require(name and "\\" not in name and "\0" not in name and not path.is_absolute()
            and path.as_posix() == name and ".." not in path.parts, "Unsafe ZIP path")
    return path


def verify_archive(path: Path) -> dict:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    with zipfile.ZipFile(path) as archive:
        if not archive.comment:
            return {"status": "unsigned_or_repacked", "origin_attested": False,
                    "message": "未找到程序签名：可能是手工打包、重新压缩或旧版生成的 ZIP。"}
        envelope = parse(archive.comment)
        require(isinstance(envelope, dict), "Invalid archive signature envelope")
        if envelope.get("format") != "maa-blackflow-auto-archive":
            return {"status": "unsigned_or_repacked", "origin_attested": False,
                    "message": "ZIP 没有可识别的采集程序签名。"}
        require(envelope.get("schema_version") == 1 and envelope.get("algorithm") == "Ed25519"
                and envelope.get("origin_attested") is False, "Unsupported archive signature format")
        for field, size in (("public_key", 64), ("signature", 128), ("archive_sha512", 128)):
            require(isinstance(envelope.get(field), str) and re.fullmatch(r"[0-9a-f]{" + str(size) + "}", envelope[field]),
                    "Invalid signature field: " + field)

        total_size = path.stat().st_size
        end_offset = total_size - len(archive.comment) - 22
        require(end_offset >= 0, "Truncated ZIP end record")
        with path.open("rb") as stream:
            stream.seek(end_offset)
            end = stream.read(22)
            require(end[:4] == b"PK\x05\x06" and struct.unpack_from("<H", end, 20)[0] == len(archive.comment),
                    "Invalid ZIP end record or trailing bytes")
            require(struct.unpack_from("<HH", end, 4) == (0, 0), "Multi-volume ZIP is unsupported")
            stream.seek(0)
            digest = hashlib.sha512()
            remaining = end_offset + 20
            while remaining:
                chunk = stream.read(min(1024 * 1024, remaining))
                require(chunk, "Truncated ZIP")
                digest.update(chunk)
                remaining -= len(chunk)
            digest.update(b"\0\0")
        require(digest.hexdigest() == envelope["archive_sha512"], "ZIP bytes changed after automatic packaging")
        try:
            Ed25519PublicKey.from_public_bytes(bytes.fromhex(envelope["public_key"])).verify(
                bytes.fromhex(envelope["signature"]), DOMAIN + digest.hexdigest().encode("ascii"))
        except Exception as error:
            raise ValueError("Invalid local Ed25519 signature") from error

        entries = archive.infolist()
        require(len(entries) <= 65535 and len(entries) == struct.unpack_from("<H", end, 10)[0], "Invalid ZIP entry count")
        names = set()
        for entry in entries:
            safe_name(entry.filename)
            require(entry.filename not in names and not entry.is_dir() and not entry.flag_bits & 1
                    and (entry.external_attr >> 16) & 0o170000 != 0o120000,
                    "Duplicate, directory, encrypted, or linked ZIP entry")
            names.add(entry.filename)
        require(sum(entry.file_size for entry in entries) <= MAX_TOTAL_BYTES, "Archive exceeds verification size limit")
        indexes = [entry for entry in entries if len(PurePosixPath(entry.filename).parts) == 2
                   and PurePosixPath(entry.filename).name == "integrity.json"]
        require(len(indexes) == 1 and indexes[0].file_size <= MAX_INDEX_BYTES, "Missing or oversized integrity manifest")
        index_entry = indexes[0]
        index = parse(archive.read(index_entry))
        root = PurePosixPath(index_entry.filename).parts[0]
        require(root.startswith("run-") and index.get("run_directory") == root, "Run directory mismatch")
        require(index.get("schema_version") == 1 and index.get("format") == "maa-blackflow-local-integrity"
                and index.get("hash_algorithm") == "SHA-512" and index.get("origin_attested") is False
                and index.get("started_from_beginning") is True
                and index.get("public_key") == envelope["public_key"], "Invalid integrity manifest identity")
        expected = {}
        require(isinstance(index.get("files"), list), "Missing file index")
        for entry in index["files"]:
            name = entry["path"]
            safe_name(name)
            require(name not in expected and name != "integrity.json", "Duplicate or recursive file index")
            require(type(entry["size"]) is int and 0 <= entry["size"] <= MAX_TOTAL_BYTES
                    and isinstance(entry["sha512"], str) and re.fullmatch(r"[0-9a-f]{128}", entry["sha512"]),
                    "Invalid file digest")
            expected[name] = entry
        require(names == {root + "/" + name for name in expected} | {index_entry.filename},
                "ZIP files do not match the integrity manifest")
        for name, entry in expected.items():
            info = archive.getinfo(root + "/" + name)
            require(info.file_size == entry["size"], "File size mismatch: " + name)
            digest = hashlib.sha512()
            count = 0
            with archive.open(info) as stream:
                while chunk := stream.read(1024 * 1024):
                    count += len(chunk)
                    require(count <= entry["size"], "Expanded file exceeds its declared size")
                    digest.update(chunk)
            require(count == entry["size"] and digest.hexdigest() == entry["sha512"], "File hash mismatch: " + name)

        for name in ("manifest.json", "run-events.jsonl", "run.log", "replay.html", "replay-data.js"):
            require(name in expected, "Required run file is missing: " + name)
        require(expected["manifest.json"]["size"] <= MAX_INDEX_BYTES, "Collector manifest is too large")
        collector = parse(archive.read(root + "/manifest.json"))
        require(collector.get("collector_version") == index.get("collector_version"), "Collector version mismatch")
        count = 0
        previous_elapsed = -1
        first_action = last_action = ""
        fresh_start = False
        with archive.open(root + "/run-events.jsonl") as stream:
            while line := stream.readline(MAX_INDEX_BYTES + 1):
                require(len(line) <= MAX_INDEX_BYTES and line.endswith(b"\n"), "Oversized or incomplete event line")
                event = parse(line)
                count += 1
                require(event.get("schema_version") == 1 and event.get("sequence") == count, "Event sequence gap")
                elapsed = event.get("elapsed_ms")
                require(type(elapsed) is int and elapsed >= 0 and elapsed >= previous_elapsed, "Event clock moved backwards")
                previous_elapsed = elapsed
                require(event.get("timestamp") and event.get("level") in {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"}
                        and event.get("action"), "Incomplete event metadata")
                last_action = event["action"]
                if count == 1:
                    first_action = last_action
                fresh_start |= last_action == "run.start_confirmed"
                image = event.get("image", {}).get("path")
                if image:
                    safe_name(image)
                    require(image in expected and PurePosixPath(image).suffix.lower() in {".jpg", ".jpeg"},
                            "Missing or invalid image reference")
        require(count == index.get("event_count") and first_action == "run.started" and last_action == "run.ended"
                and fresh_start, "Run is incomplete or was started from the middle")
        return {"status": "valid_local_signature", "origin_attested": False, "run_directory": root,
                "collector_version": index["collector_version"], "file_count": len(expected), "event_count": count,
                "message": "逐文件校验、完整开局记录和本地签名通过；不代表官方客户端来源证明。"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = verify_archive(args.archive)
        code = 0 if result["status"] == "valid_local_signature" else 2
    except Exception as error:
        result = {"status": "invalid", "origin_attested": False, "message": str(error)}
        code = 1
    print(json.dumps(result, ensure_ascii=False, indent=2) if args.json else result["message"])
    return code


if __name__ == "__main__":
    raise SystemExit(main())
