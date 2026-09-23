"""Shared production evidence crosses archive verification and consumer admission."""
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-difficulty/events.json'


def evidence():
    rows = json.loads(FIXTURE.read_text('utf-8'))
    for i, row in enumerate(rows, 1):
        row.update(sequence=i, elapsed_ms=i, timestamp='2026-09-23T00:00:00Z', level='INFO')
    return rows


def test_difficulty_evidence_keeps_existing_archive_signature_contract():
    spec = importlib.util.spec_from_file_location('difficulty_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    fixture = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    fixture.setUp()
    try:
        events = [json.loads(line) for line in fixture.files['run-fixture/run-events.jsonl'].splitlines()]
        events = events[:2] + evidence() + events[-1:]
        for i, row in enumerate(events, 1):
            row.update(sequence=i, elapsed_ms=i, timestamp='2026-09-23T00:00:00Z')
        fixture.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in events)
        index = json.loads(fixture.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        fixture.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(fixture.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        fixture.doCleanups()


def test_consumers_keep_evidence_and_reject_unverified_collection():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('Set BLACKFLOW_ANALYSIS_ROOT to the mapped analysis workspace')
    assert FIXTURE.read_bytes() == (Path(analysis) / 'tests/fixtures/blackflow-difficulty-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline import collection_policy as policy
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    samples = evidence()
    facts = normalize(samples, {'schema_version': 1}, {}, 'synthetic-difficulty', 'events', 'revision')
    rows = facts['observations']
    assert [row['kind'] for row in rows] == ['difficulty_evidence'] * 3
    assert [row['details'] for row in rows] == [row['details'] for row in samples]
    assert all(plan(row) is None for row in samples)
    for proof in samples:
        run = [dict(action='run.started', phase='started', outcome='success', details={
                   'profile': 'automation_collection', 'difficulty': 6, 'difficulty_verification': 'required'}),
               dict(action='run.start_confirmed', phase='started', outcome='success',
                    details={'source': policy.START_TASK, 'difficulty': 6}), proof,
               dict(action='run.ended', phase='completed', outcome='abandoned')]
        check = policy.Check(policy.DEFAULT, 'events', {})
        for i, row in enumerate(run, 1):
            check.observe(dict(row, sequence=i, elapsed_ms=i, floor=0, state={'floor': 0}))
        result = check.finish()
        assert result['status'] == ('accepted' if proof['details']['verified'] else 'rejected')
    samples[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(samples, {}, {}, 'future', 'events', 'revision')
