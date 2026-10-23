"""Create a DMG while tolerating hdiutil's transient Resource busy failure."""

import argparse
from pathlib import Path
import subprocess
import time


def create_dmg(source, destination, volume, attempts=5):
    # macOS may still hold disk resources after copying/signing a large app.
    # https://github.com/actions/runner-images/issues/7522
    command = [
        "hdiutil", "create", "-volname", volume, "-srcfolder", str(source),
        "-ov", "-format", "UDZO", str(destination),
    ]
    for attempt in range(1, attempts + 1):
        subprocess.run(["sync"], check=True)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(result.stdout, end="", flush=True)
        if result.returncode == 0:
            # A partial image left behind by a failed command must never count as success.
            subprocess.run(["hdiutil", "verify", str(destination)], check=True)
            return
        if "Resource busy" not in result.stdout or attempt == attempts:
            raise subprocess.CalledProcessError(result.returncode, command, output=result.stdout)
        delay = attempt * 5
        print(f"::warning::hdiutil is busy (attempt {attempt}/{attempts}); retrying in {delay}s", flush=True)
        time.sleep(delay)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--volume", required=True)
    args = parser.parse_args()
    create_dmg(args.source, args.output, args.volume)


if __name__ == "__main__":
    main()
