"""Click diagnostics and observed start-reward candidates must not become claimed items."""
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/fixtures/BlackFlow/reward-click-events.json'
START_REWARD_FIXTURE = ROOT / 'unit_test/fixtures/BlackFlow/start-reward-events.json'


def feedback(path=FIXTURE):
    return json.loads(path.read_text('utf-8'))


@pytest.mark.parametrize('path', [FIXTURE, START_REWARD_FIXTURE], ids=['reward-click', 'start-reward'])
def test_feedback_is_additive_to_signed_archive(path):
    spec = importlib.util.spec_from_file_location('reward_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + feedback(path) + events[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp='2026-09-15T00:00:00Z')
        fixture.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in events)
        index = json.loads(fixture.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        result = module.verify_run.verify_archive(fixture.resign(refresh_event_digest=True))
        assert result['status'] == 'valid_local_signature'
    finally:
        fixture.doCleanups()


def test_consumer_preserves_feedback_without_claims_or_recognition():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('Set BLACKFLOW_ANALYSIS_ROOT to the mapped analysis workspace for cross-consumer checks')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = feedback()
    assert [e['outcome'] for e in events] == ['no_progress'] * 3 + ['page_changed']
    assert all(plan(e) is None for e in events)
    facts = normalize(events, {'schema_version': 1}, {}, 'synthetic-feedback', 'events', 'revision')
    rows = [r for r in facts['observations'] if r['kind'] == 'reward_evidence']
    assert len(rows) == len(events)
    assert all(r['status'] == 'unparsed' for r in rows)
    assert [r['details'] for r in rows] == [e['details'] for e in events]
    assert not any(r['kind'] in {'processing_task', 'movement', 'node_resolution'} for r in facts['observations'])
    events[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(events, {}, {}, 'future', 'events', 'revision')


def test_consumer_preserves_start_candidates_without_claiming_unselected_rewards():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('Set BLACKFLOW_ANALYSIS_ROOT to the mapped analysis workspace for cross-consumer checks')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = feedback(START_REWARD_FIXTURE)
    observed = events[0]['details']
    assert observed['recognition_scope'] == 'visible_title_roi'
    assert observed['candidate_count'] == 5
    assert observed['candidates'][1]['text'] == '强裸骏鹰'
    assert observed['candidates'][1]['canonical'] == '襁褓骏鹰'
    assert observed['candidates'][4]['canonical'] is None
    selected = [e for e in events if e['action'] == 'start.reward.selected']
    assert len(selected) == 1
    assert selected[0]['outcome'] == 'confirmed'
    assert selected[0]['details']['selected_index'] == observed['planned_selection']['detected_index']
    assert any(e['outcome'] == 'no_titles' for e in events)
    assert all(plan(e) is None for e in events)
    facts = normalize(events, {'schema_version': 1}, {}, 'synthetic-start-reward', 'events', 'revision')
    rows = [r for r in facts['observations'] if r['kind'] == 'reward_evidence']
    assert len(rows) == len(events)
    assert all(r['status'] == 'unparsed' for r in rows)
    assert [r['details'] for r in rows] == [e['details'] for e in events]
    assert not any(r['kind'] in {'processing_task', 'movement', 'node_resolution'} for r in facts['observations'])
    # Old archives with no candidate events remain usable; their missing candidates are not negative samples.
    legacy = normalize([{'schema_version': 1, 'sequence': 1, 'action': 'run.started',
                         'timestamp': '2026-09-23T00:00:00Z',
                         'state': {}, 'details': {}}],
                       {'schema_version': 1}, {}, 'legacy', 'events', 'revision')
    assert not any(r['kind'] == 'reward_evidence' for r in legacy['observations'])
    events[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(events, {}, {}, 'future', 'events', 'revision')
