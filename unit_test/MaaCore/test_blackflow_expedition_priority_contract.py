"""Fixed-first expedition evidence coexists with historical randomized choices."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'unit_test/MaaCore/fixtures/blackflow-expedition/20261025'


def test_priority_archives_accept_independent_v1_verifier():
    spec = importlib.util.spec_from_file_location('priority_archive_helpers', ROOT / 'unit_test/MaaCore/test_blackflow_expedition_contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.FIXTURES = FIXTURES
    module.test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope()


def test_shared_consumer_keeps_fixed_choices_and_cancellations_distinct():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO must name the mapped analysis workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-expedition-priority-events.json').read_bytes() == (FIXTURES / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import combine, report
    samples = json.loads((FIXTURES / 'producer-events.json').read_bytes())
    assert len(samples) == 8
    for case in samples:
        events = case['events']
        confirmed = [e for e in events if e['action'] == 'node.expedition.dispatch_confirmed']
        planned = [e for e in events if e['action'] == 'node.expedition.choice_planned']
        for event in planned:
            assert event['details']['selected_option_index_from_top'] == 1
            assert event['details']['selection_policy'] == 'fixed_first_option'
            assert event['details']['operator_selection_policy'] == 'unpromoted_core_then_ethan_then_gummy'
        images = {e['image']['path']: {'sha256': 'fixture-image', 'path': e['image']['path']}
                  for e in events if e.get('image')}
        facts = normalize(events, {'schema_version': 1}, images, 'expedition-priority', 'events', 'revision')
        results = []
        for obs in facts['observations']:
            if obs['kind'] != 'processing_task':
                continue
            task = obs['processing']
            results.append(dict(observation_id=obs['observation_id'], input_digest=obs['input_digest'],
                                family=task['family'], status='recognized', data=combine(task, []), review_reasons=[]))
        dispatch, = report(facts['observations'], results)['expeditions']
        assert dispatch['status'] == ('confirmed' if confirmed else 'cancelled')
        if confirmed:
            assert dispatch['selected_option_index_from_top'] == 1
            assert dispatch['operator_name'] in {'凯尔希·思衡托', '伊桑', '古米'}
        else:
            assert not planned and dispatch['operator_name'] is None
            cancellation, = [e for e in events if e['action'] == 'node.expedition.cancelled']
            assert cancellation['details']['leave_option_index_from_top'] == 3
        future = copy.deepcopy(events)
        future[0]['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize(future, {}, {}, 'future', 'events', 'revision')
