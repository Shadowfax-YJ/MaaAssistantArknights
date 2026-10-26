"""Real replay journals retain modal/cleanup evidence and the refreshed inventory."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-overload/20261026'


def events():
    return json.loads((FIXTURE / 'producer-events.json').read_bytes())


def evidence_paths(rows):
    paths = set()
    for row in rows:
        if row.get('image'):
            paths.add(row['image']['path'])
        details = row.get('details', {})
        paths.update(details[key] for key in ('snapshot_file', 'captured_image_file', 'overlay_image_file') if details.get(key))
        paths.update(image['file'] for image in details.get('evidence_images', []))
    return paths


def test_original_modal_and_complete_handoff_are_preserved():
    provenance = json.loads((FIXTURE / 'provenance.json').read_bytes())
    original = provenance['original']
    assert hashlib.sha256((FIXTURE / original['file']).read_bytes()).hexdigest() == original['sha256']
    for file in provenance['sample_files']:
        assert hashlib.sha256((FIXTURE / file['file']).read_bytes()).hexdigest() == file['sha256']
    rows = events()
    opening = next(i for i, e in enumerate(rows) if e['details'].get('input_task', '').endswith('MovementInventoryOverloadOpen'))
    discard = next(i for i, e in enumerate(rows) if e['phase'] == 'discard-selected')
    cleared = next(i for i, e in enumerate(rows) if e['details'].get('overload_cleared'))
    complete = next(i for i, e in enumerate(rows) if e['action'] == 'inventory.depart-overload' and e['phase'] == 'completed')
    assert opening < discard < cleared < complete
    assert rows[discard]['details']['selected_name'] == '气垫底座'
    handoff = rows[complete]
    assert handoff['details']['resume_task'].endswith('DirectDepartHuntedResume')
    assert handoff['state']['movement_inventory_refresh_required'] is False
    assert [item['movement'] for item in handoff['state']['movement_instances']] == ['rogue_6_scrap_M_03']
    for path in evidence_paths(rows):
        assert (FIXTURE / path).is_file()
    scan = next(row for row in rows if row['action'] == 'diagnostic.processing_item_observation')
    snapshot = json.loads((FIXTURE / scan['details']['snapshot_file']).read_bytes())
    assert snapshot['floor'] == 3
    assert snapshot['scan_complete'] and snapshot['scan_scope'] == 'whole_inventory'
    assert [item['name'] for item in snapshot['items']] == ['标准引擎']
    assert len(snapshot['evidence_images']) == 2


def test_consumer_does_not_turn_cleanup_into_a_reward_or_visit():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped consumer')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    rows = events()
    images = {path: {'path': path, 'sha256': hashlib.sha256((FIXTURE / path).read_bytes()).hexdigest()}
              for path in evidence_paths(rows) if path.endswith('.jpg')}
    result = normalize(rows, {'schema_version': 1}, images, 'depart-recovery', 'events', 'revision')
    assert not any(o['kind'] in {'reward_evidence', 'recruitment', 'node_resolution', 'movement'} for o in result['observations'])
    assert not any(o['kind'] == 'processing_task' for o in result['observations'])
    future = copy.deepcopy(rows)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')


def test_original_modal_and_recovery_journal_accept_independent_v1_signature():
    spec = importlib.util.spec_from_file_location('depart_archive_builder', ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
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
            if (FIXTURE / path).exists():
                image = (FIXTURE / path).read_bytes()
                archive.files['run-fixture/' + path] = image
                if path not in indexed:
                    index['files'].append({'path': path, 'size': len(image), 'sha512': hashlib.sha512(image).hexdigest()})
                    indexed.add(path)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()
