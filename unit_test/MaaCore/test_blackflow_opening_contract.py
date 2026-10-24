"""Opening diagnostics keep the signed v1 format without inventing recruitment."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-opening/20261024'


def samples():
    return json.loads((FIXTURE / 'opening-diagnostic-events.json').read_bytes())


def test_opening_failure_image_accepts_independent_signed_v1_verifier():
    spec = importlib.util.spec_from_file_location('opening_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + samples() + original[-1:]
        for sequence, event in enumerate(rows, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in rows)
        image = (FIXTURE / 'wrong-vouchers.jpg').read_bytes()
        image_path = samples()[0]['image']['path']
        archive.files['run-fixture/' + image_path] = image
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        index['files'].append({'path': image_path, 'size': len(image), 'sha512': hashlib.sha512(image).hexdigest()})
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()


def test_consumer_preserves_opening_image_without_reward_or_recruitment_facts():
    import pytest
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped analysis workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-opening-diagnostic-events.json').read_bytes() == (FIXTURE / 'opening-diagnostic-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = samples()
    assert all(plan(event) is None for event in events)
    facts = normalize(events, {'schema_version': 1}, {}, 'opening-contract', 'events', 'revision')
    assert any(row['kind'] == 'image_evidence' for row in facts['observations'])
    assert not any(row['kind'] in {'processing_task', 'reward_evidence', 'movement', 'node_resolution', 'recruitment'}
                   for row in facts['observations'])
    future = copy.deepcopy(events)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')
