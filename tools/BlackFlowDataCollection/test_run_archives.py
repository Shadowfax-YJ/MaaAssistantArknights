"""Independent verifier regression tests using a ZIP produced by the C++ archiver."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("verify_run", ROOT / "tools/VerifyBlackFlowRunArchive.py")
verify_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify_run)
FIXTURE = ROOT / "unit_test/fixtures/BlackFlow/signed-run.zip"


class RunArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name) / "modified.zip"
        with zipfile.ZipFile(FIXTURE) as archive:
            self.files = {entry.filename: archive.read(entry) for entry in archive.infolist()}
            self.comment = archive.comment

    def repack(self, comment=b""):
        with zipfile.ZipFile(self.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in self.files.items():
                archive.writestr(name, data)
            archive.comment = comment
        return self.output

    def resign(self, refresh_event_digest=False):
        # Deliberately use an attacker-controlled key: file/semantic checks must still work,
        # and a valid result must never claim official provenance.
        key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        public = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex()
        index = json.loads(self.files["run-fixture/integrity.json"])
        index["public_key"] = public
        if refresh_event_digest:
            data = self.files["run-fixture/run-events.jsonl"]
            for entry in index["files"]:
                if entry["path"] == "run-events.jsonl":
                    entry.update(size=len(data), sha512=hashlib.sha512(data).hexdigest())
        self.files["run-fixture/integrity.json"] = json.dumps(index).encode()
        self.repack()
        digest = hashlib.sha512(self.output.read_bytes()).hexdigest()
        envelope = {"schema_version": 1, "format": "maa-blackflow-auto-archive", "algorithm": "Ed25519",
                    "origin_attested": False, "public_key": public, "archive_sha512": digest,
                    "signature": key.sign(verify_run.DOMAIN + digest.encode()).hex()}
        with zipfile.ZipFile(self.output, "a") as archive:
            archive.comment = json.dumps(envelope).encode()
        return self.output

    def test_cpp_archive_and_signature_are_valid(self):
        result = verify_run.verify_archive(FIXTURE)
        self.assertEqual(result["status"], "valid_local_signature")
        self.assertFalse(result["origin_attested"])
        self.assertEqual(result["event_count"], 3)

    def test_additive_employ_capture_keeps_v1_archive_compatible(self):
        fixture = ROOT / 'unit_test/MaaCore/fixtures/blackflow-employ'
        capture = json.loads((fixture / 'capture-event.json').read_text('utf-8'))
        events = [json.loads(line) for line in self.files['run-fixture/run-events.jsonl'].splitlines()]
        capture['sequence'] = 3
        capture['elapsed_ms'] = events[1]['elapsed_ms']
        events[-1]['sequence'] = 4
        events.insert(2, capture)
        self.files['run-fixture/run-events.jsonl'] = b''.join(
            (json.dumps(event, ensure_ascii=False) + '\n').encode() for event in events)
        image_path = capture['image']['path']
        image = (fixture / 'run-20261016-165116-426279-3995.jpg').read_bytes()
        self.files['run-fixture/' + image_path] = image
        index = json.loads(self.files['run-fixture/integrity.json'])
        index['event_count'] = 4
        index['files'].append({'path': image_path, 'size': len(image), 'sha512': hashlib.sha512(image).hexdigest()})
        self.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        result = verify_run.verify_archive(self.resign(refresh_event_digest=True))
        self.assertEqual(result['status'], 'valid_local_signature')
        self.assertEqual(result['event_count'], 4)

    def test_manual_repack_has_no_program_signature(self):
        self.assertEqual(verify_run.verify_archive(self.repack())["status"], "unsigned_or_repacked")

    def test_opening_failure_diagnostic_and_image_remain_v1_compatible(self):
        spec = importlib.util.spec_from_file_location('opening_contract',
            ROOT / 'unit_test/MaaCore/test_blackflow_opening_contract.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.test_opening_failure_image_accepts_independent_signed_v1_verifier()

    def test_cargo_decline_receipt_and_images_remain_v1_compatible(self):
        spec = importlib.util.spec_from_file_location('cargo_decline_contract',
            ROOT / 'unit_test/MaaCore/test_blackflow_cargo_decline_contract.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.test_actual_decline_frames_accept_independent_signed_v1_verifier()

    def test_opening_reward_lifecycle_keeps_v1_archive_compatible(self):
        for name in ('start-reward-events.json', 'start-reward-lifecycle-events.json'):
            with self.subTest(fixture=name):
                opening = json.loads((ROOT / 'unit_test/fixtures/BlackFlow' / name).read_text('utf-8'))
                events = [json.loads(line) for line in self.files['run-fixture/run-events.jsonl'].splitlines()]
                # Each sample combines synthetic scenarios, not another real run.
                events = events[:2] + opening + events[-1:]
                for sequence, event in enumerate(events, 1):
                    event['sequence'] = sequence
                    event['elapsed_ms'] = sequence
                    event['timestamp'] = events[0]['timestamp']
                self.files['run-fixture/run-events.jsonl'] = b''.join(
                    (json.dumps(event, ensure_ascii=False) + '\n').encode() for event in events)
                index = json.loads(self.files['run-fixture/integrity.json'])
                index['event_count'] = len(events)
                self.files['run-fixture/integrity.json'] = json.dumps(index).encode()
                result = verify_run.verify_archive(self.resign(refresh_event_digest=True))
                self.assertEqual(result['status'], 'valid_local_signature')
                self.assertEqual(result['event_count'], len(events))
                self.assertFalse(result['origin_attested'])

    def test_inventory_scan_and_possible_explanation_are_additive_v1_metadata(self):
        sample_path = ROOT / 'unit_test/MaaCore/fixtures/blackflow-dlc-movement/inventory-scan-contract.json'
        samples = json.loads(sample_path.read_text('utf-8'))
        for name in ('complete_with_eye', 'complete_without_eye', 'interrupted'):
            with self.subTest(name=name):
                events = [json.loads(line) for line in self.files['run-fixture/run-events.jsonl'].splitlines()]
                events[1].setdefault('state', {}).update(samples[name]['state'])
                events[1].setdefault('details', {}).update({
                    'inventory_scan': samples[name]['evidence'],
                    'reveal_consistency': samples['extra_merchant_explanation'],
                })
                self.files['run-fixture/run-events.jsonl'] = b''.join(
                    (json.dumps(event, ensure_ascii=False) + '\n').encode() for event in events)
                result = verify_run.verify_archive(self.resign(refresh_event_digest=True))
                self.assertEqual(result['status'], 'valid_local_signature')
                self.assertEqual(result['event_count'], 3)

    def test_sale_refresh_cycles_and_legacy_unknown_keep_v1_archive_compatible(self):
        sample = ROOT / 'unit_test/fixtures/BlackFlow/store-sale-refresh-events.json'
        added = json.loads(sample.read_text('utf-8'))
        events = [json.loads(line) for line in self.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + added + events[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp=events[0]['timestamp'])
        self.files['run-fixture/run-events.jsonl'] = b''.join(
            (json.dumps(event, ensure_ascii=False) + '\n').encode() for event in events)
        index = json.loads(self.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        self.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        result = verify_run.verify_archive(self.resign(refresh_event_digest=True))
        self.assertEqual(result['status'], 'valid_local_signature')
        self.assertEqual(result['event_count'], len(events))

    def test_copied_signature_does_not_authenticate_a_repack(self):
        with self.assertRaisesRegex(ValueError, "ZIP bytes changed"):
            verify_run.verify_archive(self.repack(self.comment))

    def test_modified_zip_bytes_are_rejected(self):
        data = bytearray(FIXTURE.read_bytes())
        data[60] ^= 1
        self.output.write_bytes(data)
        with self.assertRaises(ValueError):
            verify_run.verify_archive(self.output)

    def test_signature_corruption_is_rejected(self):
        envelope = json.loads(self.comment)
        envelope["signature"] = "00" * 64
        self.output.write_bytes(FIXTURE.read_bytes())
        with zipfile.ZipFile(self.output, "a") as archive:
            archive.comment = json.dumps(envelope).encode()
        with self.assertRaisesRegex(ValueError, "signature"):
            verify_run.verify_archive(self.output)

    def test_file_hash_still_checked_with_valid_signature(self):
        self.files["run-fixture/run.log"] = b"changed human log\n"
        with self.assertRaisesRegex(ValueError, "File (size|hash) mismatch"):
            verify_run.verify_archive(self.resign())

    def test_added_file_still_checked_with_valid_signature(self):
        self.files["run-fixture/extra.txt"] = b"extra"
        with self.assertRaisesRegex(ValueError, "files do not match"):
            verify_run.verify_archive(self.resign())

    def test_mid_run_capture_rejected_even_when_file_hashes_match(self):
        self.files["run-fixture/run-events.jsonl"] = self.files["run-fixture/run-events.jsonl"].replace(
            b"run.start_confirmed", b"map.observed")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            verify_run.verify_archive(self.resign(refresh_event_digest=True))

    def test_valid_self_issued_signature_is_not_official_attestation(self):
        result = verify_run.verify_archive(self.resign())
        self.assertEqual(result["status"], "valid_local_signature")
        self.assertFalse(result["origin_attested"])


if __name__ == "__main__":
    unittest.main()
