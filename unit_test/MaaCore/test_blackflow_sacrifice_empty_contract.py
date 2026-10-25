"""Confirmed empty and unknown picker recovery remain diagnostics, never completed exchanges."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'unit_test/MaaCore/fixtures/blackflow-sacrifice/20261025'


def test_empty_picker_archives_accept_independent_v1_verifier():
    spec = importlib.util.spec_from_file_location('sacrifice_archive_helpers', ROOT / 'unit_test/MaaCore/test_blackflow_expedition_contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.FIXTURES = FIXTURES
    module.test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope()


def test_shared_consumer_keeps_empty_and_unknown_out_of_exchange_results():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required')
    assert (Path(analysis) / 'tests/fixtures/blackflow-sacrifice-empty-events.json').read_bytes() == (FIXTURES / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan, combine, report
    for case in json.loads((FIXTURES / 'producer-events.json').read_bytes()):
        events = case['events']
        recoveries = [event for event in events if event['action'] == 'node.event.capture' and event['phase'] == 'picker-recovery']
        assert len(recoveries) == 1
        recovery, = recoveries
        expected = 'no_exchangeable_items' if case['case'] == 'sacrifice-confirmed-empty' else 'retry_limit_reached'
        assert recovery['details']['reason'] == expected
        assert plan(recovery)['runtime']['exchange_state'] == 'context_only'
        assert plan(recovery)['recognizers'] == []
        facts = normalize(events, {'schema_version': 1}, {}, 'empty-picker', 'events', 'revision')
        assert not any(obs['kind'] == 'reward_evidence' for obs in facts['observations'])
        results = []
        for obs in facts['observations']:
            if obs['kind'] != 'processing_task':
                continue
            task = obs['processing']
            results.append(dict(observation_id=obs['observation_id'], input_digest=obs['input_digest'],
                                family=task['family'], status='recognized', data=combine(task, []), review_reasons=[]))
        assert report(facts['observations'], results)['lost_and_gain_pairs'] == []
        future = copy.deepcopy(events)
        future[0]['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize(future, {}, {}, 'future', 'events', 'revision')
