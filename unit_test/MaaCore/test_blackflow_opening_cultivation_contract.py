"""Opening cultivation is separate from the optional start-reward choice pool."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/fixtures/BlackFlow/opening-cultivation-events.json'
IMAGE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-opening-cultivation/20261021/ingots-5.jpg'


def test_cultivation_events_and_original_image_remain_valid_in_a_signed_archive():
    spec = importlib.util.spec_from_file_location('cultivation_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + json.loads(FIXTURE.read_text('utf-8')) + events[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp='2026-10-21T06:31:56Z')
        fixture.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in events)
        fixture.files['run-fixture/images/opening-cultivation.jpg'] = IMAGE.read_bytes()
        index = json.loads(fixture.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        image = IMAGE.read_bytes()
        index['files'].append({'path': 'images/opening-cultivation.jpg', 'size': len(image),
                               'sha512': hashlib.sha512(image).hexdigest()})
        fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(fixture.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        fixture.doCleanups()


def test_consumer_preserves_cultivation_content_without_adding_a_start_choice():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('Set BLACKFLOW_ANALYSIS_ROOT to the mapped analysis workspace')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = json.loads(FIXTURE.read_text('utf-8'))
    assert [e['outcome'] for e in events] == ['recognized', 'pending', 'confirmed']
    assert events[0]['details']['items'] == [{'name': '源石锭', 'amount': 5}]
    assert all(e['details']['source'] == 'opening_cultivation' for e in events)
    assert {e['details']['observation_index'] for e in events} == {1}
    assert not any(e['action'].startswith('start.reward.') for e in events)
    assert all(plan(e) is None for e in events)
    result = normalize(events, {'schema_version': 1}, {}, 'cultivation', 'events', 'revision')
    rewards = [r for r in result['observations'] if r['kind'] == 'reward_evidence']
    assert len(rewards) == 3
    assert [r['details'] for r in rewards] == [e['details'] for e in events]
    assert len([r for r in result['observations'] if r['kind'] == 'image_evidence']) == 1
    assert not any(r['kind'] in {'movement', 'node_resolution', 'processing_task'} for r in result['observations'])
    legacy = normalize([{'schema_version': 1, 'sequence': 1, 'timestamp': '2026-10-21T06:31:56Z',
                         'action': 'run.started', 'state': {}, 'details': {}}],
                       {'schema_version': 1}, {}, 'legacy', 'events', 'revision')
    assert not any(r['kind'] == 'reward_evidence' for r in legacy['observations'])
    events[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(events, {}, {}, 'future', 'events', 'revision')
