"""The new click diagnostics must not become claimed items or extra OCR jobs."""
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/fixtures/BlackFlow/reward-click-events.json'


def feedback():
    return json.loads(FIXTURE.read_text('utf-8'))


def test_feedback_is_additive_to_signed_archive():
    spec = importlib.util.spec_from_file_location('reward_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + feedback() + events[-1:]
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
