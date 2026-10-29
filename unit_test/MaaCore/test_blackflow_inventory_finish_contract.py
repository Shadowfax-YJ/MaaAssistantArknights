"""Ordinary cleanup receipts, complete rescan and independent archive compatibility."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-inventory-finish/20261028'


def events():
    return json.loads((FIXTURE / 'producer-events.json').read_bytes())


def evidence_paths(rows):
    paths = set()
    for row in rows:
        if row.get('image'):
            paths.add(row['image']['path'])
        details = row.get('details', {})
        paths.update(details[key] for key in ('snapshot_file', 'captured_image_file', 'overlay_image_file')
                     if details.get(key))
        paths.update(image['file'] for image in details.get('evidence_images', []))
    return paths


def test_original_failure_and_rescan_pixels_are_preserved():
    provenance = json.loads((FIXTURE / 'provenance.json').read_bytes())
    assert provenance['source_run'] == 'run-20261028-175027-941189'
    for entry in provenance['files']:
        assert hashlib.sha256((FIXTURE / entry['file']).read_bytes()).hexdigest() == entry['sha256']


def test_original_full_inventory_tail_rebound_pixels_are_preserved():
    fixture = ROOT / 'unit_test/MaaCore/fixtures/blackflow-inventory/20261029-instance002'
    provenance = json.loads((fixture / 'provenance.json').read_bytes())
    assert provenance['source_run'] == 'MAA-002/run-20261029-190705-320729'
    assert provenance['artifact'] == 'BF-A18-137'
    assert len(provenance['files']) == 9
    for entry in provenance['files']:
        assert hashlib.sha256((fixture / entry['file']).read_bytes()).hexdigest() == entry['sha256']
    instances = provenance['expected_movement_instances']
    assert len(instances) == 8
    assert sum(item['movement'] == 'rogue_6_scrap_M_08' for item in instances) == 2


def test_success_receipt_precedes_complete_rescan_and_next_movement():
    rows = events()
    discarded, = [e for e in rows if e['action'] == 'inventory.overload.capture' and e['phase'] == 'discard-selected']
    after = next(e for e in rows if e['action'] == 'inventory.overload.capture' and e['phase'] == 'after-discard')
    closed = next(e for e in rows if e['action'] == 'inventory.overload.capture'
                  and e['phase'] == 'returned-to-map' and e['details'].get('page') == 'map')
    complete = next(e for e in rows if e['action'] == 'inventory.overload-check'
                    and e['phase'] == 'completed' and e['details'].get('cleanup_performed'))
    scanned, = [e for e in rows if e['action'] == 'diagnostic.processing_item_observation']
    moved, = [e for e in rows if e['action'] == 'move.confirm' and e['phase'] == 'completed']
    assert discarded['details']['selected_name'] == '枯苔藓球'
    assert (discarded['details']['capacity_count'], after['details']['capacity_count']) == (13, 12)
    assert after['details']['overload_cleared']
    assert discarded['sequence'] < after['sequence'] < closed['sequence'] < complete['sequence'] < scanned['sequence'] < moved['sequence']
    assert not any(e['action'] in {'task.finished', 'run.ended'} for e in rows)
    snapshot = json.loads((FIXTURE / 'producer' / scanned['details']['snapshot_file']).read_bytes())
    assert snapshot['scan_complete'] and snapshot['scan_scope'] == 'whole_inventory'
    assert len(snapshot['evidence_images']) == 6
    assert moved['state']['movement_inventory_refresh_required'] is False
    assert moved['state']['movement_instances']
    assert all(item['movement'] != 'rogue_6_scrap_M_04' for item in moved['state']['movement_instances'])
    original = json.loads((FIXTURE / 'provenance.json').read_bytes())['expected_movement_instances']
    assert [(item['movement'], item['remaining_charges']) for item in snapshot['items']] == [
        (item['movement'], item['remaining_charges']) for item in original]
    for path in evidence_paths(rows):
        assert (FIXTURE / 'producer' / path).is_file(), path


def test_consumer_cleanup_diagnostics_are_not_acquisitions_or_visits():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped consumer')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported

    rows = events()
    images = {path: {'path': path, 'sha256': hashlib.sha256((FIXTURE / 'producer' / path).read_bytes()).hexdigest()}
              for path in evidence_paths(rows) if path.endswith('.jpg')}
    cleanup = [e for e in rows if e['action'].startswith('inventory.overload')]
    result = normalize(cleanup, {'schema_version': 1}, images, 'inventory-finish', 'events', 'revision')
    assert not any(o['kind'] in {'reward_evidence', 'recruitment', 'node_resolution', 'movement', 'processing_task'}
                   for o in result['observations'])
    normalize(rows, {'schema_version': 1}, images, 'inventory-finish', 'events', 'revision')
    future = copy.deepcopy(rows)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')


def test_independent_v1_signature_accepts_complete_cleanup_evidence():
    spec = importlib.util.spec_from_file_location('inventory_finish_archives',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + events() + original[-1:]
        for sequence, row in enumerate(rows, 1):
            row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
            row.pop('run_uuid', None)
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(row).encode() + b'\n' for row in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        indexed = {entry['path'] for entry in index['files']}
        for path in evidence_paths(rows):
            source = FIXTURE / 'producer' / path
            if source.is_file():
                data = source.read_bytes()
                archive.files['run-fixture/' + path] = data
                if path not in indexed:
                    index['files'].append({'path': path, 'size': len(data), 'sha512': hashlib.sha512(data).hexdigest()})
                    indexed.add(path)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()
