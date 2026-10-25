"""Overload progress is evidence, never a replacement inventory or acquisition."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-overload/20261025'


def test_overload_originals_are_unmodified():
    for frame in json.loads((FIXTURE / 'provenance.json').read_bytes())['files']:
        assert hashlib.sha256((FIXTURE / frame['file']).read_bytes()).hexdigest() == frame['sha256']


def test_shared_consumer_keeps_progress_images_without_inventory_or_item_facts():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT is required')
    assert (Path(analysis) / 'tests/fixtures/blackflow-overload-events.json').read_bytes() == (FIXTURE / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = json.loads((FIXTURE / 'producer-events.json').read_bytes())
    assert all(plan(event) is None for event in events)
    assert any(event['details'].get('overload_cleared') is True for event in events)
    assert any(event['details'].get('selected_name') for event in events)
    assert all('natural_items' not in event['details'] and 'scan_complete' not in event['details'] for event in events)
    facts = normalize(events, {}, {}, 'overload-contract', 'events', 'revision')
    assert sum(row['kind'] == 'image_evidence' for row in facts['observations']) == len(events)
    assert not any(row['kind'] in {'processing_task', 'reward_evidence', 'movement', 'node_resolution', 'recruitment'}
                   for row in facts['observations'])
    future = copy.deepcopy(events); future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')


def test_overload_images_and_optional_fields_accept_the_independent_signed_envelope():
    spec = importlib.util.spec_from_file_location('overload_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        events = json.loads((FIXTURE / 'producer-events.json').read_bytes())
        rows = original[:2] + events + original[-1:]
        for seq, row in enumerate(rows, 1):
            row.update(sequence=seq, elapsed_ms=seq, timestamp=original[0]['timestamp'])
            row.pop('run_uuid', None)
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(row).encode() + b'\n' for row in rows)
        index = json.loads(archive.files['run-fixture/integrity.json']); index['event_count'] = len(rows)
        indexed = {row['path'] for row in index['files']}
        for event in events:
            path = event['image']['path']
            assert path.startswith('images/')
            name = 'cleared-map.jpg' if event['details'].get('overload_cleared') else 'overloaded-inventory.jpg'
            image = (FIXTURE / name).read_bytes()
            archive.files['run-fixture/' + path] = image
            if path not in indexed:
                index['files'].append({'path': path, 'size': len(image), 'sha512': hashlib.sha512(image).hexdigest()})
                indexed.add(path)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()
