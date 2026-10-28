"""Frozen production journals from original ending/network image replays."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-ui-recovery/20261028'


def events(case):
    return json.loads((FIXTURE / f'producer-{case}-events.json').read_bytes())


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


def test_original_images_have_recorded_digests():
    provenance = json.loads((FIXTURE / 'provenance.json').read_bytes())
    assert len(provenance['endings']) == 3
    for entry in provenance['files']:
        assert hashlib.sha256((FIXTURE / entry['file']).read_bytes()).hexdigest() == entry['sha256']


@pytest.mark.parametrize('case', ['expedition', 'network'])
def test_production_journal_references_all_saved_evidence(case):
    rows = events(case)
    assert len({row['sequence'] for row in rows}) == len(rows)
    for path in evidence_paths(rows):
        assert (FIXTURE / f'producer-{case}' / path).is_file(), path


def test_ending_is_saved_before_dispatch_and_node_settlement():
    rows = events('expedition')
    ending = next(row for row in rows if row['action'] == 'node.event.capture'
                  and row['details'].get('secondary_event_layout') == 'ending')
    dispatched, = [row for row in rows if row['action'] == 'node.expedition.dispatch_confirmed']
    settled, = [row for row in rows if row['action'] == 'telemetry.BlackFlowNodeResolution']
    refresh = next(row for row in rows if row['action'] == 'map.refresh' and row['phase'] == 'completed')
    assert ending['sequence'] < dispatched['sequence'] < settled['sequence'] < refresh['sequence']
    assert ending['image']['path']
    assert ending['details']['ending_title'] == '未涉足之树'
    assert '你的同伴出发了' in ending['details']['story_text']
    assert dispatched['details']['confirmation_basis'] == 'secondary_event'
    assert not any(row['action'] == 'run.ended' for row in rows)


def test_network_recovery_has_original_busy_scene_then_verified_move():
    from PIL import Image, ImageChops, ImageStat

    rows = events('network')
    waits = [row for row in rows if row['action'] == 'movement.network-wait']
    assert [row['phase'] for row in waits] == ['started', 'resumed']
    original = Image.open(FIXTURE / 'network-panel.jpg')
    saved = Image.open(FIXTURE / 'producer-network' / waits[0]['image']['path'])
    # The production logger encodes JPEG again; compare pixels, not the JPEG stream.
    assert saved.size == original.size
    assert max(ImageStat.Stat(ImageChops.difference(original, saved)).mean) < 0.2
    completed, = [row for row in rows if row['action'] == 'move.confirm' and row['phase'] == 'completed']
    assert waits[0]['sequence'] < waits[1]['sequence'] < completed['sequence']
    assert not any(row.get('outcome') == 'movement_selection_failed' for row in rows)


@pytest.mark.parametrize('case', ['expedition', 'network'])
def test_consumer_preserves_receipt_without_inventing_network_business_facts(case):
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped consumer')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan, combine, report

    rows = events(case)
    images = {path: {'path': path, 'sha256': hashlib.sha256(
        (FIXTURE / f'producer-{case}' / path).read_bytes()).hexdigest()}
        for path in evidence_paths(rows) if path.endswith('.jpg')}
    normalized = normalize(rows, {'schema_version': 1}, images, 'recovery', 'events', 'revision')
    observations = normalized['observations']
    assert not any(o['kind'] in {'reward_evidence', 'recruitment'} for o in observations)
    results = []
    for obs in observations:
        if obs['kind'] == 'processing_task':
            task = obs['processing']
            results.append({'observation_id': obs['observation_id'], 'input_digest': obs['input_digest'],
                'family': task['family'], 'status': 'recognized', 'data': combine(task, []),
                'floor': task['floor'], 'floor_source': task['floor_source'], 'review_reasons': []})
    processed = report(observations, results)
    if case == 'expedition':
        dispatch, = processed['expeditions']
        assert dispatch['status'] == 'confirmed' and dispatch['choice_source'] == 'direct_record'
        assert dispatch['operator_name'] == '凯尔希·思衡托'
        ending = next(row for row in rows if row['action'] == 'node.event.capture'
                      and row['details'].get('secondary_event_layout') == 'ending')
        task = plan(ending)
        assert task['recognizers'] == []
        assert task['runtime']['story_text'] == ending['details']['story_text']
    else:
        assert not processed['expeditions']
        assert not any(o['kind'] in {'processing_task', 'node_resolution', 'movement'} for o in observations)
        assert all(plan(row) is None for row in rows if row['action'] == 'movement.network-wait')
    future = copy.deepcopy(rows)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')


@pytest.mark.parametrize('case', ['expedition', 'network'])
def test_independent_v1_signature_accepts_new_optional_evidence(case):
    spec = importlib.util.spec_from_file_location('ui_recovery_archives',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + events(case) + original[-1:]
        for sequence, row in enumerate(rows, 1):
            row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
            row.pop('run_uuid', None)
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(row).encode() + b'\n' for row in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        indexed = {entry['path'] for entry in index['files']}
        for path in evidence_paths(rows):
            source = FIXTURE / f'producer-{case}' / path
            if source.is_file():
                data = source.read_bytes()
                archive.files['run-fixture/' + path] = data
                if path not in indexed:
                    index['files'].append({'path': path, 'size': len(data),
                                           'sha512': hashlib.sha512(data).hexdigest()})
                    indexed.add(path)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()
