"""A decline receipt records option four, never an acquired reward or extra visit."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-encounter/20261024/cargo-decline'


def test_actual_decline_frames_accept_independent_signed_v1_verifier():
    spec = importlib.util.spec_from_file_location('decline_archive_helper', ROOT / 'unit_test/MaaCore/test_blackflow_expedition_contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.FIXTURES = FIXTURE
    module.test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope()


def test_shared_decline_choice_remains_a_choice_without_reward_or_visit():
    import pytest
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO must name the mapped consumer workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-cargo-decline-events.json').read_bytes() == (FIXTURE / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import combine, report
    for case in json.loads((FIXTURE / 'producer-events.json').read_bytes()):
        images = {e['image']['path']: {'sha256': 'fixture', 'path': e['image']['path']}
                  for e in case['events'] if e.get('image')}
        facts = normalize(case['events'], {'schema_version': 1}, images, 'decline', 'events', 'revision')
        results = []
        for obs in facts['observations']:
            if obs['kind'] != 'processing_task':
                continue
            task = obs['processing']
            results.append({'observation_id': obs['observation_id'], 'input_digest': obs['input_digest'],
                            'family': task['family'], 'status': 'recognized', 'data': combine(task, []),
                            'review_reasons': []})
        summary = report(facts['observations'], results)
        choice, = summary['encounter_choices']
        assert choice['status'] == 'confirmed' and choice['selected_option_index'] == 4
        assert choice['selected_option'] == '没什么想买的'
        assert not any(o['kind'] in {'reward_evidence', 'encounter', 'movement', 'node_resolution', 'recruitment'} for o in facts['observations'])
        assert not summary['expeditions']
        future = copy.deepcopy(case['events'])
        future[0]['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize(future, {}, {}, 'future', 'events', 'revision')
