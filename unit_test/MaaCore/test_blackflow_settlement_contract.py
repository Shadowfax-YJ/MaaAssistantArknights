"""Settlement details are views; exit finalizes one run before the next opening."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-settlement/20261029'


def events(which='old'):
    return json.loads((FIXTURE / f'producer-{which}-events.json').read_bytes())


def test_original_pages_and_template_crops_keep_provenance():
    from PIL import Image

    provenance = json.loads((FIXTURE / 'provenance.json').read_bytes())
    for item in provenance['files']:
        assert hashlib.sha256((FIXTURE / item['file']).read_bytes()).hexdigest() == item['sha256']
    source = Image.open(FIXTURE / 'summary.png').convert('RGB').resize((1280, 720), Image.Resampling.LANCZOS)
    for name, rect in [('SettlementNext', (1195, 320, 1256, 400)), ('SettlementPage', (42, 12, 245, 63))]:
        template = Image.open(ROOT / f'resource/template/Roguelike/BlackFlow/BlackFlow@Roguelike@{name}.png').convert('RGB')
        expected = source.crop(rect)
        assert template.size == expected.size and template.tobytes() == expected.tobytes()


def test_detail_statistics_exit_and_next_run_have_ordered_independent_evidence():
    old, new = events(), events('new')
    detail, = [e for e in old if e['action'] == 'settlement.detail.capture']
    stats, = [e for e in old if e['phase'] == 'statistics-ready']
    confirm, = [e for e in old if e['phase'] == 'statistics-before-confirm']
    entrance = next(e for e in old if e['phase'] == 'navigation-ready' and e['details'].get('destination') == 'entrance')
    ended, = [e for e in old if e['action'] == 'run.ended']
    assert detail['sequence'] < stats['sequence'] < confirm['sequence'] < entrance['sequence'] < ended['sequence']
    assert stats['details']['statistics']['score'] == 868
    assert stats['details']['statistics']['game_pass'] is False
    assert stats['details']['recognition_scope'] == 'settlement_statistics_rois'
    assert detail['details']['scope'] == 'run_settlement'
    assert detail['details']['acquisition_status'] == 'not_an_acquisition'
    assert not detail['details'].get('attribution')
    assert sum(e['action'] == 'run.started' for e in new) == 1
    assert sum(e['action'] == 'run.start_confirmed' for e in new) == 1
    opening, = [e for e in new if e['action'] == 'start.opening.roles.observed']
    assert opening['image']['path']
    assert opening['details']['recognized_header'] == '选择招募组合'
    assert not any(e['action'] == 'run.ended' for e in new)
    for label, rows in [('old', old), ('new', new)]:
        for e in rows:
            if e.get('image'):
                assert (FIXTURE / f'producer-{label}' / e['image']['path']).is_file()


def test_consumer_does_not_recognize_old_item_details_as_new_rewards():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped consumer')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan

    rows = events()
    images = {e['image']['path']: {'path': e['image']['path'], 'sha256': hashlib.sha256(
        (FIXTURE / 'producer-old' / e['image']['path']).read_bytes()).hexdigest()}
        for e in rows if e.get('image')}
    result = normalize(rows, {'schema_version': 1}, images, 'settlement', 'events', 'revision')
    assert not any(o['kind'] in {'reward_evidence', 'recruitment', 'node_resolution', 'processing_task'}
                   for o in result['observations'])
    assert all(plan(e) is None for e in rows if e['action'].startswith('settlement.'))
    future = copy.deepcopy(rows)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')


def test_independent_v1_archive_accepts_settlement_diagnostics_and_originals():
    spec = importlib.util.spec_from_file_location('settlement_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + [e for e in events() if e['action'] not in {'run.started', 'run.ended'}] + original[-1:]
        for sequence, row in enumerate(rows, 1):
            row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
            row.pop('run_uuid', None)
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        indexed = {entry['path'] for entry in index['files']}
        for e in rows:
            if not e.get('image'):
                continue
            path = e['image']['path']
            source = FIXTURE / 'producer-old' / path
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
