"""A run purchase policy is metadata, never evidence of owning or receiving an item."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-store-policy/20261025/producer-policies.json'


@pytest.mark.parametrize('disabled', [False, True])
def test_run_purchase_policy_keeps_signed_archive_compatible(disabled):
    spec = importlib.util.spec_from_file_location('store_policy_archive',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        events = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        policy = json.loads(FIXTURE.read_bytes())[int(disabled)]
        for event in events:
            event.setdefault('state', {})['store_purchase_policy'] = policy
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in events)
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()


def test_policy_does_not_create_or_remove_consumer_item_facts():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped analysis workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-store-policies.json').read_bytes() == FIXTURE.read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    policies = json.loads(FIXTURE.read_bytes())
    for policy in policies:
        event = {'schema_version': 1, 'sequence': 1, 'timestamp': '2026-10-25T00:00:00Z',
                 'action': 'run.started', 'phase': 'started', 'outcome': 'success',
                 'details': {'profile': 'automation_collection'},
                 'state': {'profile': 'automation_collection', 'store_purchase_policy': policy}}
        assert plan(event) is None
        old = copy.deepcopy(event)
        del old['state']['store_purchase_policy']
        manifest = {'schema_version': 1, 'run_uuid': 'store-policy-contract'}
        assert normalize([event], manifest, {}, 'policy', 'events', 'revision') == normalize([old], manifest, {}, 'policy', 'events', 'revision')
        event['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize([event], {}, {}, 'future', 'events', 'revision')
