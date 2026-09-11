"""Cross-consumer compatibility cases for the producer-owned verifier bundle."""
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
if ROOT.name == 'unit_test':
    ROOT = ROOT.parent
spec = importlib.util.spec_from_file_location('verifier', ROOT / 'tools/VerifyBlackFlowRunArchive.py')
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def test_signed_fixture_and_content_tampering(tmp_path):
    fixture = ROOT / 'unit_test/fixtures/BlackFlow/signed-run.zip'
    assert verifier.verify_archive(fixture)['status'] == 'valid_local_signature'
    data = bytearray(fixture.read_bytes())
    data[50] ^= 1
    changed = tmp_path / 'tampered.zip'
    changed.write_bytes(data)
    with pytest.raises(ValueError):
        verifier.verify_archive(changed)


def test_future_signed_envelope_has_distinct_exit_code(tmp_path):
    future = tmp_path / 'future.zip'
    with zipfile.ZipFile(future, 'w') as archive:
        archive.writestr('placeholder', b'future')
        archive.comment = json.dumps({'format': 'maa-blackflow-auto-archive', 'schema_version': 99}).encode()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/VerifyBlackFlowRunArchive.py'),
        str(future), '--json'], capture_output=True)
    assert result.returncode == 3
    assert json.loads(result.stdout)['status'] == 'unsupported'


def test_bundle_records_exact_verifier_and_expected_fixture(tmp_path):
    result = subprocess.run([sys.executable, str(ROOT / 'tools/BuildBlackFlowContract.py'),
                            '--output', str(tmp_path / 'bundle')], capture_output=True)
    assert result.returncode == 0, result.stderr
    manifest = json.loads(result.stdout)
    assert manifest['expected']['fixtures/signed-run.zip'] == 'valid_local_signature'
    assert (tmp_path / 'bundle/VerifyBlackFlowRunArchive.py').read_bytes() == (ROOT / 'tools/VerifyBlackFlowRunArchive.py').read_bytes()


def test_additive_manifest_identity_and_future_raw_contract():
    tests_spec = importlib.util.spec_from_file_location('archive_fixture_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(tests_spec)
    tests_spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        manifest = json.loads(fixture.files['run-fixture/manifest.json'])
        manifest.update(contract_id='maa.blackflow.raw', contract_version=1,
                        run_uuid='12345678-1234-4234-9234-123456789abc')
        def signed():
            data = json.dumps(manifest).encode()
            fixture.files['run-fixture/manifest.json'] = data
            index = json.loads(fixture.files['run-fixture/integrity.json'])
            for entry in index['files']:
                if entry['path'] == 'manifest.json':
                    entry.update(size=len(data), sha512=hashlib.sha512(data).hexdigest())
            fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
            return fixture.resign()
        assert verifier.verify_archive(signed())['status'] == 'valid_local_signature'
        manifest['run_uuid'] = 'invalid'
        with pytest.raises(ValueError, match='UUID'):
            verifier.verify_archive(signed())
        manifest['contract_version'] = 99
        with pytest.raises(verifier.UnsupportedContract):
            verifier.verify_archive(signed())
    finally:
        fixture.doCleanups()


def test_recruitment_and_purchase_evidence_are_additive_signed_contract_fields():
    tests_spec = importlib.util.spec_from_file_location('evidence_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(tests_spec)
    tests_spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        evidence = json.loads((ROOT / 'unit_test/fixtures/BlackFlow/recruitment-store-events.json').read_bytes())
        events = events[:2] + evidence + events[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp='2026-09-12T00:00:00Z',
                         schema_version=1, level='INFO')
        fixture.files['run-fixture/run-events.jsonl'] = b''.join(
            json.dumps(event, ensure_ascii=False).encode() + b'\n' for event in events)
        manifest = json.loads(fixture.files['run-fixture/manifest.json'])
        manifest['capabilities'] = ['recruitment-choice-v1', 'store-purchase-evidence-v2']
        data = json.dumps(manifest).encode()
        fixture.files['run-fixture/manifest.json'] = data
        index = json.loads(fixture.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        for entry in index['files']:
            if entry['path'] == 'manifest.json':
                entry.update(size=len(data), sha512=hashlib.sha512(data).hexdigest())
        fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        result = verifier.verify_archive(fixture.resign(refresh_event_digest=True))
        assert result['status'] == 'valid_local_signature'
        assert result['event_count'] == 11
        assert result['origin_attested'] is False
    finally:
        fixture.doCleanups()
