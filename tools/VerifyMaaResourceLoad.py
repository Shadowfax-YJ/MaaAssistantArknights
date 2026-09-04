#!/usr/bin/env python3
"""Load a packaged MaaCore and verify that its bundled resources are usable."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path, help="Directory containing the resource folder")
    parser.add_argument("--library", type=Path, help="MaaCore library path; inferred on Windows when omitted")
    parser.add_argument("--scratch", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()

    root = args.root.resolve()
    library = args.library.resolve() if args.library else root / "MaaCore.dll"
    if not library.is_file():
        print(f"MaaCore library is missing: {library}", file=sys.stderr)
        return 2
    if not (root / "resource").is_dir():
        print(f"Resource directory is missing: {root / 'resource'}", file=sys.stderr)
        return 2

    # MaaCore keeps its log file open until the process exits. Run the actual
    # load in a child process so the parent can delete the isolated user
    # directory after the DLL and logger have been unloaded.
    if args.scratch is None:
        with tempfile.TemporaryDirectory(prefix="maa-resource-smoke-") as scratch:
            return subprocess.call(
                [
                    sys.executable,
                    str(Path(__file__).resolve()),
                    "--root",
                    str(root),
                    "--library",
                    str(library),
                    "--scratch",
                    scratch,
                ]
            )

    dll_directory = None
    if platform.system() == "Windows":
        dll_directory = os.add_dll_directory(str(library.parent))

    try:
        scratch = args.scratch.resolve()
        previous_directory = Path.cwd()
        os.chdir(scratch)
        try:
            core = ctypes.CDLL(str(library))
            core.AsstSetUserDir.argtypes = (ctypes.c_char_p,)
            core.AsstSetUserDir.restype = ctypes.c_bool
            core.AsstLoadResource.argtypes = (ctypes.c_char_p,)
            core.AsstLoadResource.restype = ctypes.c_bool
            if not core.AsstSetUserDir(str(scratch).encode("utf-8")):
                print(f"AsstSetUserDir rejected smoke-test directory: {scratch}", file=sys.stderr)
                return 2
            loaded = bool(core.AsstLoadResource(str(root).encode("utf-8")))
        finally:
            os.chdir(previous_directory)
    finally:
        if dll_directory is not None:
            dll_directory.close()

    print(f"RESOURCE_LOAD_OK={loaded}")
    return 0 if loaded else 1


if __name__ == "__main__":
    raise SystemExit(main())
