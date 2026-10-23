"""Duel preparation choices and screenshots are additive evidence, not battle outcomes."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-duel'


def samples():
    return json.loads((FIXTURE / 'producer-events.json').read_bytes())


def test_preparation_tiers_match_both_real_tile_maps():
    payloads = json.loads((FIXTURE / 'preparation.json').read_bytes())
    assert len(payloads) == 12
    for index in range(0, len(payloads), 2):
        selected, before = payloads[index:index + 2]
        details = before['details']
        assert selected['details']['selected_tier'] == details['selected_tier']
        assert selected['details']['deployment_verification'] == 'not_deployed'
        assert details['deployment_verification'] == 'preparation_counter'
        assert not selected['details']['deployed_operators']
        stage = details['stage_name']
        tile_path = next((ROOT / 'resource/Arknights-Tile-Pos').glob(
            'ro6_duel_' + ('1-' if stage == '搏杀' else '2-') + '*.json'))
        tiles = json.loads(tile_path.read_bytes())['tiles']
        x = {'left': 1, 'middle': 4, 'right': 7}[details['selected_tier']]
        expected = {(x, 3)} if stage == '搏杀' else {(x, 3), (x + 1, 3)}
        assert {tuple(p['location']) for p in details['preparation_plan']} == expected
        assert {tuple(p['location']) for p in details['deployed_operators']} == expected
        assert all(tiles[y][x]['buildableType'] == 1 for x, y in expected)


def test_duel_evidence_uses_the_existing_v1_signed_archive():
    spec = importlib.util.spec_from_file_location(
        'duel_archive_builder', ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + samples() + original[-1:]
        added_images = []
        for sequence, row in enumerate(rows, 1):
            row.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
            if row.get('image'):
                # Synthetic image bytes exercise envelope integrity, not recognition or geometry.
                image = b'duel-frame-fixture'
                archive.files['run-fixture/' + row['image']['path']] = image
                added_images.append({'path': row['image']['path'], 'size': len(image),
                                     'sha512': hashlib.sha512(image).hexdigest()})
        archive.files['run-fixture/run-events.jsonl'] = b''.join(
            json.dumps(row, ensure_ascii=False).encode() + b'\n' for row in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        index['files'].extend(added_images)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        verified = module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))
        assert verified['status'] == 'valid_local_signature'
    finally:
        archive.tearDown()


def test_consumer_keeps_choices_without_ocr_or_claiming_combat_rewards():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required for independent consumer checks')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    current = samples()
    legacy = copy.deepcopy([row for row in current if row['action'] == 'node.event.capture'])
    for row in legacy:
        row['phase'] = 'event-choice'
        row['details'] = {'event_name': row['details']['event_name']}
    for rows in (current, legacy):
        facts = normalize(rows, {'schema_version': 1}, {}, 'duel-contract', 'events', 'revision')
        assert all(plan(row) is None for row in rows)
        selections = [row for row in facts['observations'] if row['kind'] == 'reward_evidence']
        assert len(selections) == (6 if rows is current else 0)
        for row in selections:
            assert row['status'] == 'unparsed'
            assert row['details']['selected_tier'] in ('left', 'middle', 'right')
        assert all(row['kind'] in ('encounter', 'map', 'reward_evidence', 'image_evidence')
                   for row in facts['observations'])
        assert sum(row['kind'] == 'map' for row in facts['observations']) == 1
    future = copy.deepcopy(current)
    future[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')
