"""Opening transition images remain diagnostics alongside the normal reward popup capture."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-opening/20261025'


def samples():
    return json.loads((FIXTURE / 'opening-popup-transition-events.json').read_bytes())


def test_opening_popup_transition_accepts_independent_signed_v1_verifier():
    spec = importlib.util.spec_from_file_location('opening_popup_archive_builder', ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
    archive.setUp()
    try:
        original = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
        rows = original[:2] + samples() + original[-1:]
        images = []
        for sequence, event in enumerate(rows, 1):
            event.update(sequence=sequence, elapsed_ms=sequence, timestamp=original[0]['timestamp'])
            if not event['action'].startswith('start.opening.support.'):
                continue
            name = event['image']['path']
            image = (FIXTURE / Path(name).name).read_bytes()
            archive.files['run-fixture/' + name] = image
            images.append(dict(path=name, size=len(image), sha512=hashlib.sha512(image).hexdigest()))
        archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(e).encode() + b'\n' for e in rows)
        index = json.loads(archive.files['run-fixture/integrity.json'])
        index['event_count'] = len(rows)
        index['files'].extend(images)
        archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
        assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
    finally:
        archive.doCleanups()


def test_consumer_keeps_transition_as_images_without_extra_items_or_ocr():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT must name the mapped analysis workspace')
    assert (Path(analysis) / 'tests/fixtures/blackflow-opening-popup-transition-events.json').read_bytes() == (FIXTURE / 'opening-popup-transition-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import plan
    events = samples()
    assert [e['action'] for e in events] == ['start.opening.support.confirming', 'start.opening.support.transition']
    assert all(plan(e) is None for e in events)
    facts = normalize(events, {'schema_version': 1}, {}, 'opening-popup', 'events', 'revision')
    assert sum(e['kind'] == 'image_evidence' for e in facts['observations']) == 2
    assert not any(e['kind'] in {'processing_task', 'reward_evidence', 'movement', 'node_resolution', 'recruitment'} for e in facts['observations'])
    future = copy.deepcopy(events)
    future[0]['schema_version'] = 999
    with pytest.raises(Unsupported):
        normalize(future, {}, {}, 'future', 'events', 'revision')
