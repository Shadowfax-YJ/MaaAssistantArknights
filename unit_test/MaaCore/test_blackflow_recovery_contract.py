"""Recovery evidence must remain diagnostic, never a shelf or recruitment fact."""
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


def diagnostics():
    events = json.loads((ROOT / 'unit_test/MaaCore/fixtures/blackflow-recovery/diagnostics.json').read_text('utf-8'))
    for sequence, event in enumerate(events, 1):
        event.update(sequence=sequence, elapsed_ms=sequence, timestamp='2026-09-23T00:00:00Z')
    return events


def test_recovery_events_remain_valid_in_a_signed_archive():
    spec = importlib.util.spec_from_file_location('recovery_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + diagnostics() + events[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp='2026-09-23T00:00:00Z')
        fixture.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in events)
        index = json.loads(fixture.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(fixture.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        fixture.doCleanups()


def test_consumer_does_not_treat_recovery_as_business_evidence():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('Set BLACKFLOW_ANALYSIS_ROOT to the mapped analysis workspace')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = diagnostics()
    assert all(plan(event) is None for event in events)
    result = normalize(events, {'schema_version': 1}, {}, 'synthetic-recovery', 'events', 'revision')
    assert not any(row['kind'] in {'processing_task', 'movement', 'node_resolution', 'reward_evidence'}
                   for row in result['observations'])
    events[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(events, {}, {}, 'future', 'events', 'revision')
