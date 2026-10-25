"""The same production-recorder chains go through the consumer and v1 verifier."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-multistage/20261025'


def samples():
    return json.loads((FIXTURE / 'producer-events.json').read_bytes())


def test_all_configured_links_and_original_failure_are_covered():
    events = json.loads((ROOT / 'resource/roguelike/BlackFlow/encounter/default.json').read_bytes())['stage']
    links = {(event['name'], event['next_event']) for event in events if event.get('next_event')}
    links |= {(event['name'], group['next_event']) for event in events
              for group in event.get('choice_groups', []) if group.get('next_event')}
    assert len(links) == 12
    assert {(case['names'][0], case['names'][1]) for case in samples()} == links
    assert len(samples()) == 15  # All three same-name loops also continue to a third page.
    original = json.loads((FIXTURE / 'original-failure.json').read_bytes())
    for frame in original['images']:
        assert hashlib.sha256((FIXTURE / frame['file']).read_bytes()).hexdigest() == frame['sha256']
    case = next(case for case in samples() if case['names'] == [original['root'], original['child']])
    confirmed = [event for event in case['events'] if event['action'] == 'node.encounter.choice.confirmed']
    assert [event['details']['option'] for event in confirmed] == original['selected_options']


def test_consumer_preserves_real_step_names_without_extra_visits_or_acquisitions():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_ROOT')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_ROOT is required')
    assert (Path(analysis) / 'tests/fixtures/blackflow-multistage-events.json').read_bytes() == (FIXTURE / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.normalize import normalize, Unsupported
    from lubiao_pipeline.processing import combine, report
    for index, case in enumerate(samples()):
        events = case['events']
        images = {event['image']['path']: {'path': event['image']['path'],
                  'sha256': hashlib.sha256((FIXTURE / 'cases' / str(index) / event['image']['path']).read_bytes()).hexdigest()}
                  for event in events if event.get('image')}
        facts = normalize(events, {'schema_version': 1}, images, f'multistage-{index}', 'events', 'revision')
        results = []
        for obs in facts['observations']:
            if obs['kind'] != 'processing_task':
                continue
            task = obs['processing']
            assert task['family'] == 'encounter_choice_evidence' and task['recognizers'] == []
            results.append({'observation_id': obs['observation_id'], 'input_digest': obs['input_digest'],
                            'family': task['family'], 'status': 'recognized', 'data': combine(task, []),
                            'review_reasons': []})
        organized = report(facts['observations'], results)
        choices = organized['encounter_choices']
        assert len(choices) == len(case['names'])
        assert all(choice['status'] == 'confirmed' and choice['selected_option_index'] == 1 for choice in choices)
        assert sorted(choice['event_name'] for choice in choices) == sorted(case['names'])
        assert len({choice['source_context']['transaction_id'] for choice in choices}) == 1
        assert len({choice['source_context']['node'] for choice in choices}) == 1
        assert not any(obs['kind'] in {'encounter', 'reward_evidence', 'movement', 'node_resolution', 'recruitment'}
                       for obs in facts['observations'])
        assert not organized['expeditions'] and not organized['lost_and_gain_pairs']
        future = copy.deepcopy(events)
        future[0]['schema_version'] = 999
        with pytest.raises(Unsupported):
            normalize(future, {}, {}, 'future', 'events', 'revision')


def test_independent_signed_v1_envelope_accepts_every_chain():
    spec = importlib.util.spec_from_file_location('multistage_archive_builder',
        ROOT / 'tools/BlackFlowDataCollection/test_run_archives.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for case_index, case in enumerate(samples()):
        archive = module.RunArchiveTests('test_cpp_archive_and_signature_are_valid')
        archive.setUp()
        try:
            base = [json.loads(line) for line in archive.files['run-fixture/run-events.jsonl'].splitlines()]
            rows = copy.deepcopy(base[:2] + case['events'] + base[-1:])
            images = {}
            for seq, row in enumerate(rows, 1):
                row.update(sequence=seq, elapsed_ms=seq, timestamp=base[0]['timestamp'])
                row.pop('run_uuid', None)
                if row.get('image') and row['action'] == 'node.event.capture':
                    path = row['image']['path']
                    assert path.startswith('collection-popups/floor-1/node-')
                    frame = (FIXTURE / 'cases' / str(case_index) / path).read_bytes()
                    archive.files['run-fixture/' + path] = frame
                    images[path] = {'path': path, 'size': len(frame), 'sha512': hashlib.sha512(frame).hexdigest()}
            archive.files['run-fixture/run-events.jsonl'] = b''.join(json.dumps(row).encode() + b'\n' for row in rows)
            index = json.loads(archive.files['run-fixture/integrity.json'])
            index['event_count'] = len(rows)
            index['files'].extend(images.values())
            archive.files['run-fixture/integrity.json'] = json.dumps(index).encode()
            assert module.verify_run.verify_archive(archive.resign(refresh_event_digest=True))['status'] == 'valid_local_signature'
        finally:
            archive.tearDown()
