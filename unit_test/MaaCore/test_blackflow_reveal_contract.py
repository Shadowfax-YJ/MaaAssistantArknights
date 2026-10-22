"""Production reveal diagnostics remain diagnostic across v1 archive and consumer boundaries."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-reveal/20261022/producer-events.json'


def samples():
    return json.loads(FIXTURE.read_bytes())


def test_production_reveal_bounds_distinguish_unknown_and_confirmed():
    rows = samples()
    assert len(rows) == 9
    assert sum(row['details']['consistency'] == '未确定' for row in rows) == 5
    for row in rows:
        details = row['details']
        required = set(details['expected_revealed_nodes'])
        possible = set(details['possible_revealed_nodes'])
        uncertain = set(details['uncertain_revealed_nodes'])
        assert required <= possible
        assert uncertain == possible - required
        assert not details['missing_revealed_nodes']
        assert not details['unexpected_revealed_nodes']
        assert bool(uncertain) == (details['consistency'] == '未确定')
        if details.get('completed_event_reveal_confirmed'):
            origin = details['entered_event_node']
            assert origin in required and origin in details['observed_revealed_nodes']


def test_reveal_diagnostics_keep_existing_v1_archive_signature():
    spec = importlib.util.spec_from_file_location(
        'reveal_archive_builder', ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        events = original[:2] + samples() + original[-1:]
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
        archive.files['run-fixture/run-events.jsonl'] = b''.join(
            json.dumps(event, ensure_ascii=False).encode() + b'\n' for event in events)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(events)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        verified = module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))
        assert verified['status'] == 'valid_local_signature'
        assert verified['event_count'] == len(events)
    finally:
        archive.tearDown()


def test_consumer_accepts_new_and_legacy_diagnostics_without_creating_reveal_facts():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required for the independent consumer check')
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    current = samples()
    legacy = copy.deepcopy(current)
    for row in legacy:
        details = row['details']
        for key in ('possible_revealed_nodes', 'uncertain_revealed_nodes', 'completed_event_reveal_confirmed'):
            details.pop(key, None)
        if details['consistency'] == '未确定':
            details['consistency'] = '不一致'
    for rows in (current, legacy):
        facts = normalize(rows, {'schema_version': 1}, {}, 'reveal-contract', 'events', 'revision')
        assert all(plan(row) is None for row in rows)
        assert all(row['kind'] == 'map' and row['status'] == 'unknown' for row in facts['observations'])
    future = copy.deepcopy(current)
    future[0]['schema_version'] = 99
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')
