import base64
import copy
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, PrivateFormat, NoEncryption

import updates


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="blackflow-feed-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.version = "v1.2.3"
        self.key = Ed25519PrivateKey.generate()
        self.secret = base64.b64encode(self.key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())).decode()
        self.public = self.root / "key.txt"
        self.public.write_text(base64.b64encode(self.key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)).decode())
        self.windows = self.root / f"MAA-BlackFlow-Data-Collection-{self.version}-win-x64.zip"
        with zipfile.ZipFile(self.windows, "w") as archive:
            archive.writestr("blackflow-update.json", json.dumps({"schema_version": 1, "channel": updates.CHANNEL, "version": self.version}))
            for name in ("MAA.exe", "MaaCore.dll", "MAA.Updater.exe", "filelist.txt", "resource/blackflow-gui-defaults.json"):
                archive.writestr(name, "fixture")
        self.macos = self.root / f"MAA-BlackFlow-Data-Collection-{self.version}-macos-universal.dmg"
        self.macos.write_bytes(b"fixture disk image\0\xff")
        self.app = self.root / "MAA.app"
        (self.app / "Contents").mkdir(parents=True)
        self.info = {
            "CFBundleShortVersionString": self.version, "CFBundleVersion": "1.2.3",
            "SUPublicEDKey": self.public.read_text(), "SUFeedURL": f"{updates.FEED_ROOT}/appcast.xml",
            "BlackFlowUpdateChannel": updates.CHANNEL, "LSMinimumSystemVersion": "14.0",
        }
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps(self.info))
        self.metadata = self.root / "mac.json"
        updates.attest_macos(self.app, self.macos, self.version, self.public, self.metadata)
        self.notes = self.root / "notes.md"
        self.notes.write_text("中文说明 <test> & details", encoding="utf-8")
        self.output = self.root / "feed"

    def generate(self):
        return updates.generate(self.version, self.windows, self.macos, self.metadata, self.notes, self.public, self.output, self.secret)

    def test_sign_and_verify_actual_bytes(self):
        manifest = self.generate()
        rss = ET.parse(self.output / "appcast.xml")
        item = rss.find("channel/item")
        self.assertEqual(item.find(f"{{{updates.SPARKLE}}}version").text, "1.2.3")
        self.assertEqual(item.find("description").text, self.notes.read_text(encoding="utf-8"))
        enclosure = item.find("enclosure")
        self.key.public_key().verify(base64.b64decode(enclosure.get(f"{{{updates.SPARKLE}}}edSignature")), self.macos.read_bytes())
        self.assertEqual(manifest["assets"]["win-x64"]["sha256"], updates.sha256(self.windows))
        self.assertEqual(manifest["assets"]["macos-universal"]["size"], self.macos.stat().st_size)
        self.assertNotIn(self.secret, (self.output / "latest.json").read_text(encoding="utf-8"))

    def test_wrong_signing_key(self):
        self.secret = base64.b64encode(bytes(32)).decode()
        with self.assertRaises(ValueError):
            self.generate()
        self.assertFalse(self.output.exists())

    def test_modified_dmg(self):
        self.macos.write_bytes(b"different bytes")
        with self.assertRaises(ValueError):
            self.generate()

    def test_live_configuration_rejected(self):
        with zipfile.ZipFile(self.windows, "a") as archive:
            archive.writestr("config/gui.new.json", "{}")
        with self.assertRaises(ValueError):
            self.generate()

    def test_upstream_app_rejected(self):
        self.info["SUFeedURL"] = "https://example.com/upstream.xml"
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps(self.info))
        with self.assertRaises(ValueError):
            updates.attest_macos(self.app, self.macos, self.version, self.public, self.metadata)

    def test_configure_dedicated_app(self):
        source = self.root / "src/MaaMacGui/MeoAsstMac/Info.plist"
        source.parent.mkdir(parents=True)
        source.write_bytes(plistlib.dumps({"CFBundleIdentifier": "com.hguandl.MeoAsstMac", "SUPublicEDKey": "upstream"}))
        updates.configure_macos("v1.10.0", self.public, self.root)
        result = plistlib.loads(source.read_bytes())
        self.assertEqual(result["CFBundleIdentifier"], "com.hguandl.MeoAsstMac")
        self.assertEqual(result["SUPublicEDKey"], self.public.read_text())
        self.assertTrue(result["SUVerifyUpdateBeforeExtraction"])
        self.assertIn("CURRENT_PROJECT_VERSION = 1.10.0", (source.parent.parent / "Version.xcconfig").read_text())

    def test_channel_advance(self):
        current = self.generate()
        candidate = copy.deepcopy(current)
        candidate["version"] = "v1.10.0"
        updates.validate_advance(current, candidate)
        updates.validate_advance(current, current)
        candidate["version"] = "v1.2.2"
        with self.assertRaises(ValueError):
            updates.validate_advance(current, candidate)
        candidate["version"] = current["version"]
        candidate["assets"]["win-x64"]["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            updates.validate_advance(current, candidate)

    def test_publication_reads_before_mutation(self):
        self.generate()
        with patch.object(updates, "gh", side_effect=RuntimeError("network unavailable")) as cli:
            with self.assertRaises(RuntimeError):
                updates.publish(self.version, self.windows, self.macos, self.notes, self.output, "a" * 40)
            self.assertEqual(cli.call_count, 1)
            self.assertEqual(cli.call_args.args[:2], ("release", "list"))


if __name__ == "__main__":
    unittest.main()
