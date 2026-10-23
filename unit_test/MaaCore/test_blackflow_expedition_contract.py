"""Actual producer archive envelopes remain compatible with the v1 verifier and consumer."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'unit_test/MaaCore/fixtures/blackflow-expedition/20261024'


def samples():
    return json.loads((FIXTURES / 'producer-events.json').read_bytes())


def test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope():
    spec = importlib.util.spec_from_file_location('expedition_archive_builder', ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    frame = (FIXTURES / 'evidence-frame.jpg').read_bytes()
    for case in samples():
        archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
        archive.setUp()
        try:
            original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
            rows = copy.deepcopy(original[:2] + case['events'] + original[-1:])
            images = {}
            for sequence, row in enumerate(rows, 1):
                row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
                row.pop('run_uuid', None)
                if row.get('image') and row['action'].startswith(('node.expedition.', 'node.event.')):
                    path = row['image']['path']
                    assert path.startswith('collection-popups/floor-2/node-')
                    archive.files['run-fixture/' + path] = frame
                    images[path] = {'path': path, 'size': len(frame), 'sha512': hashlib.sha512(frame).hexdigest()}
            archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(row, ensure_ascii=False).encode() + b'\n' for row in rows)
            index = json.loads(archive.files['run-fixture/integrity.json'])
            index['event_count'] = len(rows)
            index['files'].extend(images.values())
            archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
            verified = module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))
            assert verified['status'] == 'valid_local_signature'
        finally:
            archive.tearDown()


def test_shared_consumer_fixture_is_identical_and_legacy_unknowns_stay_unknown():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required for cross-repository checks')
    assert (Path(analysis) / 'tests/fixtures/blackflow-expedition-events.json').read_bytes() == (FIXTURES / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    for case in samples():
        facts = normalize(case['events'], {'schema_version': 1}, {}, 'expedition-contract', 'events', 'revision')
        tasks = [o for o in facts['observations'] if o['kind'] == 'processing_task']
        assert len(tasks) == len(case['events'])
        assert all(t['processing']['family'] == 'expedition_evidence' for t in tasks)
        assert not any(o['kind'] == 'reward_evidence' for o in facts['observations'])
        old = copy.deepcopy([e for e in case['events'] if e['action'] == 'node.event.capture'])
        for event in old:
            event['details'] = {'event_name': '未涉足之树'}
        assert all(plan(e) is None for e in old)
        old_facts = normalize(old, {'schema_version': 1}, {}, 'old-expedition', 'events', 'revision')
        assert not any(o['kind'] == 'processing_task' for o in old_facts['observations'])
        future = copy.deepcopy(case['events'])
        future[0]['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize(future, {}, {}, 'future', 'events', 'revision')
