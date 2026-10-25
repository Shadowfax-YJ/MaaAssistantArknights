"""Explicit observation links survive attribution; input evidence is not an award."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
POPUP = ROOT / 'unit_test/MaaCore/fixtures/blackflow-popup/20261025/producer-events.json'
CIV = ROOT / 'unit_test/MaaCore/fixtures/blackflow-civilization/20261025'


def consumer():
    path = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not path:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT is required')
    sys.path.insert(0, str(Path(path) / 'src'))


def test_original_frames_and_actual_recognition_recipe_are_reproducible():
    for case in json.loads((CIV / 'sources.json').read_bytes()):
        for frame in case['images']:
            assert hashlib.sha256((CIV / frame['path']).read_bytes()).hexdigest() == frame['sha256']
    recipe = json.loads((CIV / 'recognition-recipe.json').read_bytes())
    for file, expected in recipe.items():
        if file != 'tasks':
            assert hashlib.sha256((ROOT / file).read_bytes()).hexdigest() == expected
    tasks = json.loads((ROOT / 'resource/tasks/Roguelike/BlackFlow.json').read_bytes())
    for name, expected in recipe['tasks'].items():
        assert tasks['BlackFlow@Roguelike@' + name] == expected


def test_pending_and_final_share_original_sampling_reference():
    rows = json.loads(POPUP.read_bytes())
    first, final = [row['details']['evidence_sample'] for row in rows]
    assert first == final
    assert first['sampled_at_unix_ms'] > 0 and first['sample_ordinal'] > 0
    assert final['sample_state']['floor'] == 1
    assert rows[1]['details']['attributed_at_unix_ms'] >= first['sampled_at_unix_ms']


def test_floor_and_tree_attribution_keep_the_original_sampling_state():
    consumer()
    from lubiao_pipeline.normalize import normalize
    for floor in (2, 4, 6):
        rows = json.loads(POPUP.read_bytes())
        rows[1]['floor'] = rows[1]['state']['floor'] = floor
        rows[1]['timestamp'] = '2026-10-25T00:01:00Z'
        rows[1]['details']['attribution']['floor'] = floor
        facts = normalize(rows, {}, {}, 'test', 'events', str(floor))
        tasks = [r for r in facts['observations'] if r['kind'] == 'processing_task']
        assert len(tasks) == 1
        assert tasks[0]['processing']['runtime']['evidence_sample']['sample_state']['floor'] == 1
        assert tasks[0]['sequence'] == 2


def test_parent_and_child_popups_can_be_resolved_in_reverse_order_without_hash_pairing():
    consumer()
    from lubiao_pipeline.evidence_sample import popup_dispositions
    from lubiao_pipeline.normalize import normalize
    first = json.loads(POPUP.read_bytes())
    child = copy.deepcopy(first)
    for e in child:
        e['details']['evidence_sample'].update(evidence_id='popup-1-2', sample_ordinal=2)
        e['details']['evidence_sample']['sample_state']['floor'] = 6
    rows = [first[0], child[0], child[1], first[1]]
    for n, e in enumerate(rows, 1): e['sequence'] = n
    assert popup_dispositions(rows) == {4: 'final', 3: 'final'}
    tasks = [r for r in normalize(rows, {}, {}, 'test', 'events', 'reverse')['observations']
             if r['kind'] == 'processing_task']
    assert [r['processing']['runtime']['evidence_sample']['sample_state']['floor'] for r in tasks] == [6, 1]


def test_same_name_same_image_consecutive_observations_remain_distinct_but_final_retry_does_not():
    consumer()
    from lubiao_pipeline.normalize import normalize
    rows = json.loads(POPUP.read_bytes())
    duplicate = copy.deepcopy(rows[1]); duplicate['sequence'] = 3
    consecutive = copy.deepcopy(rows[1]); consecutive['sequence'] = 4
    consecutive['details']['evidence_sample'].update(evidence_id='popup-1-2', sample_ordinal=2)
    facts = normalize(rows + [duplicate, consecutive], {}, {}, 'test', 'events', 'identity')
    assert sum(r['kind'] == 'processing_task' for r in facts['observations']) == 2
    assert sum(r['kind'] == 'image_evidence' for r in facts['observations']) == 4
    assert not any(r['kind'] == 'reward_evidence' for r in facts['observations'])


def test_conflicting_sample_identity_requires_review_and_legacy_time_stays_unknown():
    consumer()
    from lubiao_pipeline.normalize import normalize
    from lubiao_pipeline.evidence_sample import sample
    rows = json.loads(POPUP.read_bytes())
    rows[1]['details']['evidence_sample']['sampled_at_unix_ms'] += 1
    facts = normalize(rows, {}, {}, 'test', 'events', 'conflict')
    assert not any(r['kind'] == 'processing_task' for r in facts['observations'])
    assert any(r.get('sample_disposition') == 'conflict' for r in facts['observations'])
    assert sample({'details': {}}) is None


def test_receipt_progress_is_organized_without_ocr_or_inventing_acquisition():
    consumer()
    from lubiao_pipeline.processing import plan
    task = plan({'action': 'node.reward.result', 'sequence': 1, 'phase': 'after-input',
                 'details': {'receipt_status': 'page_changed', 'selected_name': '种子'}})
    assert task['family'] == 'reward_receipt_evidence'
    assert task['recognizers'] == []
    assert task['runtime']['acquisition_status'] == 'unresolved'


def test_real_logger_envelopes_keep_generic_choices_and_inventory_ui_state_separate():
    consumer()
    from lubiao_pipeline.processing import plan
    fixture = ROOT / 'unit_test/MaaCore/fixtures/blackflow-encounter/20261025/producer-events.json'
    analysis = Path(os.environ['BLACKFLOW_ANALYSIS_ROOT'])
    assert fixture.read_bytes() == (analysis / 'tests/fixtures/blackflow-evidence-repair-events.json').read_bytes()
    cases = json.loads(fixture.read_bytes())
    preserved_complete = False
    for case in cases:
        for event in case['events']:
            task = plan(event)
            if task is None:
                assert event['action'].startswith('node.sacrifice.')
                continue
            assert task['recognizers'] == []
            if task['family'] == 'encounter_choice_evidence':
                if case['case'] != 'direct-event-result':
                    assert task['runtime']['acquisition_status'] == 'unresolved'
            elif task['family'] == 'reward_receipt_evidence':
                assert task['runtime']['acquisition_status'] == 'unresolved'
            elif task['runtime'].get('inventory_status') == 'complete':
                assert task['runtime']['scan_complete'] is True
                assert task['runtime']['panel_closed'] is False
                assert isinstance(task['runtime']['natural_items'], list)
                preserved_complete = True
    assert preserved_complete


def test_actual_generic_and_inventory_events_accept_the_independent_v1_signature_verifier():
    spec = importlib.util.spec_from_file_location('evidence_archive_contract',
        ROOT / 'unit_test/MaaCore/test_blackflow_expedition_contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.FIXTURES = ROOT / 'unit_test/MaaCore/fixtures/blackflow-encounter/20261025'
    module.test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope()
