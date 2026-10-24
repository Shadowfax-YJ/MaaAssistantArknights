"""Pending popup evidence is durable but does not duplicate the final item task."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-popup/20261025/producer-events.json'


def test_pending_and_final_popup_images_pass_signed_archive_validation():
    spec = importlib.util.spec_from_file_location('popup_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        events = json.loads(FIXTURE.read_bytes())
        rows = original[:2] + events + original[-1:]
        for sequence, row in enumerate(rows, 1):
            row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        image = (ROOT / 'unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-reward.jpg').read_bytes()
        for event in events:
            path = event['image']['path']
            archive.files['run-fixture/' + path] = image
            index['files'].append({'path': path, 'size': len(image), 'sha512': hashlib.sha512(image).hexdigest()})
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()


def test_consumer_keeps_both_images_but_only_one_popup_processing_task():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped analysis workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-popup-pending-events.json').read_bytes() == FIXTURE.read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = json.loads(FIXTURE.read_bytes())
    assert plan(events[0]) is None
    assert plan(events[1])['family'] == 'single_popup'
    facts = normalize(events, {'schema_version': 1}, {}, 'popup-contract', 'events', 'revision')
    assert sum(row['kind'] == 'image_evidence' for row in facts['observations']) == 2
    assert sum(row['kind'] == 'processing_task' for row in facts['observations']) == 1
    assert not any(row['kind'] in {'movement', 'node_resolution', 'recruitment', 'reward_evidence'}
                   for row in facts['observations'])
    future = copy.deepcopy(events)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')
