"""Build and publish the isolated BlackFlow update feed. Private keys never enter packages."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

CHANNEL = "blackflow-data-collection"
REPOSITORY = "Shadowfax-YJ/MaaAssistantArknights"
RELEASE_ROOT = f"https://github.com/{REPOSITORY}/releases"
FEED_ROOT = f"{RELEASE_ROOT}/download/blackflow-updates"
SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent


def numeric_version(version: str) -> str:
    if not re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version):
        raise ValueError("Expected vMAJOR.MINOR.PATCH")
    return version[1:]


def public_key(path: Path) -> bytes:
    key = base64.b64decode(path.read_text().strip(), validate=True)
    if len(key) != 32:
        raise ValueError("Invalid Ed25519 public key")
    return key


def configure_macos(version: str, key_file: Path, repository: Path = REPO_ROOT) -> None:
    build = numeric_version(version)
    key = base64.b64encode(public_key(key_file)).decode()
    plist_path = repository / "src/MaaMacGui/MeoAsstMac/Info.plist"
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    info.update(
        SUFeedURL=f"{FEED_ROOT}/appcast.xml",
        SUPublicEDKey=key,
        SUEnableAutomaticChecks=True,
        SUAutomaticallyUpdate=True,
        SUVerifyUpdateBeforeExtraction=True,
        BlackFlowUpdateChannel=CHANNEL,
    )
    with plist_path.open("wb") as stream:
        plistlib.dump(info, stream, sort_keys=False)
    (repository / "src/MaaMacGui/Version.xcconfig").write_text(
        f"MARKETING_VERSION = {version}\nCURRENT_PROJECT_VERSION = {build}\n", encoding="utf-8"
    )


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def asset(path: Path, version: str, platform: str) -> dict:
    suffix = "zip" if platform.startswith("win-") else "dmg"
    expected = f"MAA-BlackFlow-Data-Collection-{version}-{platform}.{suffix}"
    if path.name != expected or path.stat().st_size == 0:
        raise ValueError(f"Package name/size does not match {version} {platform}")
    return {
        "name": path.name,
        "url": f"{RELEASE_ROOT}/download/blackflow-{version}/{path.name}",
        "size": path.stat().st_size,
        "sha256": sha256(path),
    }


def attest_macos(app: Path, dmg: Path, version: str, key_file: Path, output: Path) -> None:
    with (app / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    key = base64.b64encode(public_key(key_file)).decode()
    if (
        info["CFBundleShortVersionString"] != version
        or info["CFBundleVersion"] != numeric_version(version)
        or info["SUPublicEDKey"] != key
        or info["SUFeedURL"] != f"{FEED_ROOT}/appcast.xml"
        or info.get("BlackFlowUpdateChannel") != CHANNEL
    ):
        raise ValueError("The macOS application does not match the release channel/version/key")
    metadata = asset(dmg, version, "macos-universal")
    metadata.update(
        version=version,
        build_version=info["CFBundleVersion"],
        minimum_system_version=info["LSMinimumSystemVersion"],
        public_key=key,
    )
    output.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def generate(
    version: str, windows: Path, macos: Path, mac_metadata: Path, notes: Path,
    key_file: Path, output: Path, private_key: str,
) -> dict:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    build = numeric_version(version)
    public = public_key(key_file)
    seed = base64.b64decode(private_key.strip(), validate=True)
    if len(seed) != 32:
        raise ValueError("The signing secret must contain a base64 Ed25519 seed (32 bytes)")
    signer = Ed25519PrivateKey.from_private_bytes(seed)
    if signer.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) != public:
        raise ValueError("Signing key does not match the key embedded in the application")
    win_asset = asset(windows, version, "win-x64")
    mac_asset = asset(macos, version, "macos-universal")
    with zipfile.ZipFile(windows) as archive:
        identity = json.loads(archive.read("blackflow-update.json"))
        if identity != {"schema_version": 1, "channel": CHANNEL, "version": version}:
            raise ValueError("Windows package identity does not match this release")
        if any(name.startswith("config/") for name in archive.namelist()):
            raise ValueError("Release packages must not ship live user configuration")
        for name in ("MAA.exe", "MaaCore.dll", "MAA.Updater.exe", "resource/blackflow-gui-defaults.json", "filelist.txt"):
            if archive.getinfo(name).file_size == 0:
                raise ValueError(f"Empty required package file: {name}")

    attestation = json.loads(mac_metadata.read_text(encoding="utf-8"))
    if (
        any(attestation.get(key) != value for key, value in mac_asset.items())
        or attestation.get("version") != version
        or attestation.get("build_version") != build
        or attestation.get("public_key") != base64.b64encode(public).decode()
    ):
        raise ValueError("macOS package attestation does not match this release")
    body = notes.read_text(encoding="utf-8")
    manifest = {
        "schema_version": 1, "channel": CHANNEL, "version": version,
        "release_url": f"{RELEASE_ROOT}/tag/blackflow-{version}",
        "release_notes": body, "assets": {"win-x64": win_asset, "macos-universal": mac_asset},
    }
    signature = base64.b64encode(signer.sign(macos.read_bytes())).decode()
    ET.register_namespace("sparkle", SPARKLE)
    rss = ET.Element("rss", {"version": "2.0"})
    channel = ET.SubElement(rss, "channel")
    ET.SubElement(channel, "title").text = "MAA BlackFlow Data Collection"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = f"黑流树海采集版 {version}"
    ET.SubElement(item, "link").text = manifest["release_url"]
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = build
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}minimumSystemVersion").text = attestation["minimum_system_version"]
    ET.SubElement(item, "pubDate").text = dt.datetime.now(dt.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")
    ET.SubElement(item, "description").text = body
    ET.SubElement(item, "enclosure", {
        "url": mac_asset["url"], "length": str(mac_asset["size"]),
        "type": "application/octet-stream", f"{{{SPARKLE}}}edSignature": signature,
    })
    output.mkdir(parents=True, exist_ok=True)
    (output / "latest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    ET.indent(rss)
    ET.ElementTree(rss).write(output / "appcast.xml", encoding="utf-8", xml_declaration=True)
    (output / "SHA256.txt").write_text(
        "".join(f"{entry['sha256']}  {entry['name']}\n" for entry in (win_asset, mac_asset)), encoding="utf-8"
    )
    return manifest


def gh(*args: str) -> str:
    result = subprocess.run(["gh", *args, "--repo", REPOSITORY], check=True, text=True, capture_output=True)
    return result.stdout


def publish(version: str, windows: Path, macos: Path, notes: Path, output: Path, commit: str) -> None:
    # No mutation until both archives, their signatures and feed contents have been generated successfully.
    manifest = json.loads((output / "latest.json").read_text(encoding="utf-8"))
    if manifest["version"] != version or manifest["channel"] != CHANNEL or manifest["schema_version"] != 1:
        raise ValueError("Feed version does not match publication")
    tag = f"blackflow-{version}"
    # A failed list/read is an error, never evidence that a release does not exist.
    releases = json.loads(gh("release", "list", "--limit", "1000", "--json", "tagName"))
    tags = {entry["tagName"] for entry in releases}
    with tempfile.TemporaryDirectory(prefix="blackflow-publication-") as temporary:
        root = Path(temporary)
        if "blackflow-updates" in tags:
            channel = json.loads(gh("release", "view", "blackflow-updates", "--json", "assets"))
            if any(entry["name"] == "latest.json" for entry in channel["assets"]):
                previous = root / "latest.json"
                gh("release", "download", "blackflow-updates", "--pattern", "latest.json", "--output", str(previous))
                current = json.loads(previous.read_text(encoding="utf-8"))
                validate_advance(current, manifest)
        if tag in tags:
            # A retry after a failed feed upload may reuse the exact released binaries.
            release = json.loads(gh("release", "view", tag, "--json", "targetCommitish,isDraft,isPrerelease"))
            if release["targetCommitish"] != commit or release["isDraft"] or release["isPrerelease"]:
                raise ValueError(f"{tag} does not match this build commit/release type")
            for package in (windows, macos, output / "SHA256.txt"):
                downloaded = root / package.name
                gh("release", "download", tag, "--pattern", package.name, "--output", str(downloaded))
                if sha256(downloaded) != sha256(package):
                    raise ValueError(f"{tag} already contains different binaries; increment the version")
        else:
            gh("release", "create", tag, str(windows), str(macos), str(output / "SHA256.txt"),
               "--target", commit, "--title", f"黑流树海采集版 {version}", "--notes-file", str(notes), "--latest=false")
    if "blackflow-updates" not in tags:
        gh("release", "create", "blackflow-updates", "--target", commit,
           "--title", "BlackFlow update channel", "--notes", "采集版自动更新清单；安装包见对应版本 Release。",
           "--prerelease", "--latest=false")
    gh("release", "upload", "blackflow-updates", str(output / "appcast.xml"), str(output / "latest.json"), "--clobber")


def validate_advance(current: dict, candidate: dict) -> None:
    if current.get("channel") != CHANNEL or current.get("schema_version") != 1:
        raise ValueError("Existing channel metadata is invalid")
    old = tuple(map(int, numeric_version(current["version"]).split(".")))
    new = tuple(map(int, numeric_version(candidate["version"]).split(".")))
    if new < old or (new == old and current["assets"] != candidate["assets"]):
        raise ValueError("Refusing to downgrade the channel or replace same-version binaries")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    configure = commands.add_parser("configure-macos")
    configure.add_argument("--version", required=True)
    attest = commands.add_parser("attest-macos")
    for name in ("version", "app", "dmg", "output"):
        attest.add_argument("--" + name, required=True, type=str if name == "version" else Path)
    build = commands.add_parser("generate")
    for name in ("windows", "macos", "mac-metadata", "notes", "output"):
        build.add_argument("--" + name, required=True, type=Path)
    build.add_argument("--version", required=True)
    build.add_argument("--publish", action="store_true")
    build.add_argument("--commit", default="")
    for command in (configure, attest, build):
        command.add_argument("--public-key", type=Path, default=HERE / "sparkle-public-key.txt")
    args = parser.parse_args()
    if args.command == "configure-macos":
        configure_macos(args.version, args.public_key)
    elif args.command == "attest-macos":
        attest_macos(args.app, args.dmg, args.version, args.public_key, args.output)
    else:
        if args.publish and not re.fullmatch(r"[0-9a-f]{40}", args.commit):
            parser.error("Publishing requires the exact 40-character build commit")
        key = os.environ.get("BLACKFLOW_SPARKLE_PRIVATE_KEY", "")
        if not key:
            parser.error("BLACKFLOW_SPARKLE_PRIVATE_KEY is required; it is never printed or packaged")
        generate(args.version, args.windows, args.macos, args.mac_metadata, args.notes, args.public_key, args.output, key)
        if args.publish:
            publish(args.version, args.windows, args.macos, args.notes, args.output, args.commit)
        print(f"Prepared BlackFlow {args.version} update feed in {args.output}")


if __name__ == "__main__":
    main()
