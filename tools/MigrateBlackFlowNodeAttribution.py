"""Merge legacy node attribution.txt into attribution.json, backing up originals.

Only unpacked run-*/collection-popups directories are changed. Stop the collector
before applying. Without --apply this validates the input and prints a preview.
"""

import argparse
import json
import os
from pathlib import Path


def plan_merge(root):
    root = root.resolve(strict=True)
    changes = []
    for text_path in sorted(root.glob("run-*/collection-popups/**/attribution.txt")):
        text_path = text_path.resolve(strict=True)
        text_path.relative_to(root)
        metadata_path = text_path.with_suffix(".json")
        metadata_path.resolve().relative_to(root)
        text_bytes = text_path.read_bytes()
        previous = metadata_path.read_bytes() if metadata_path.exists() else None
        metadata = json.loads(previous.decode("utf-8-sig")) if previous is not None else {}
        if not isinstance(metadata, dict):
            raise ValueError(f"Metadata is not an object: {metadata_path}")
        annotations = metadata.get("annotations", [])
        if not isinstance(annotations, list) or any(not isinstance(x, str) for x in annotations):
            raise ValueError(f"Invalid annotations: {metadata_path}")
        lines = [line for line in text_bytes.decode("utf-8-sig").splitlines() if line]
        metadata["annotations"] = annotations + lines
        merged = (json.dumps(metadata, ensure_ascii=False, indent=4) + "\n").encode("utf-8")
        changes.append((text_path, metadata_path, text_bytes, previous, merged, len(lines)))
    return root, changes


def apply_merge(root, changes, backup):
    for text_path, metadata_path, text_bytes, previous, merged, _ in changes:
        if text_path.read_bytes() != text_bytes or (
            metadata_path.read_bytes() if metadata_path.exists() else None
        ) != previous:
            raise RuntimeError(f"File changed after validation: {text_path}")
        saved_text = backup / text_path.relative_to(root)
        saved_text.parent.mkdir(parents=True, exist_ok=True)
        with saved_text.open("xb") as output:
            output.write(text_bytes)
        if previous is not None:
            with saved_text.with_suffix(".json").open("xb") as output:
                output.write(previous)
        temporary = metadata_path.with_name("attribution.json.merge-tmp")
        try:
            with temporary.open("xb") as output:
                output.write(merged)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, metadata_path)
            if metadata_path.read_bytes() != merged:
                raise RuntimeError(f"Merged file verification failed: {metadata_path}")
            text_path.unlink()
        except Exception:
            # Keep the original pair intact if this node could not be migrated.
            if previous is None:
                metadata_path.unlink(missing_ok=True)
            else:
                metadata_path.write_bytes(previous)
            text_path.write_bytes(text_bytes)
            raise
        finally:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, action="append", required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup-dir", type=Path)
    args = parser.parse_args()
    plans = [plan_merge(root) for root in dict.fromkeys(p.resolve() for p in args.root)]
    for root, changes in plans:
        print(f"{root}: {len(changes)} TXT files, {sum(c[-1] for c in changes)} annotation lines")
    if not args.apply:
        return
    if args.backup_dir is None:
        parser.error("--apply requires --backup-dir")
    backup = args.backup_dir.resolve()
    if any(backup == root or root in backup.parents for root, _ in plans):
        parser.error("backup directory must be outside the run roots")
    backup.mkdir(parents=True, exist_ok=False)
    (backup / "roots.json").write_text(
        json.dumps([str(root) for root, _ in plans], ensure_ascii=False, indent=2), encoding="utf-8"
    )
    for index, (root, changes) in enumerate(plans):
        apply_merge(root, changes, backup / str(index))
    print(f"Merged and verified; originals saved in {backup}")


if __name__ == "__main__":
    main()
