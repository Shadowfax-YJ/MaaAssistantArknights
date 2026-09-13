"""A reviewed correction is a derivative, preserves evidence, and rejects damage."""
import importlib.util
import copy
import json
from pathlib import Path
import sys
import zipfile

import pytest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
from ReviseBlackFlowRunArchive import revise, sha, file_sha, correct_combat, correct_landing
from VerifyBlackFlowRunArchive import verify_archive, UnsupportedContract


def fixture(tmp_path):
    source = tmp_path / 'old.zip'
    left = {'sequence': 1, 'floor': 5, 'transaction_id': 'first', 'state': {'page': {
        'node': 10, 'node_name': '黑诞', 'node_type': 'incident'}}}
    right = {'sequence': 2, 'floor': 5, 'transaction_id': 'second', 'state': {'page': {
        'node': 18446744073709551615, 'node_name': '黑诞', 'node_type': 'incident'}}}
    nodes = {'floor': 5, 'transaction_id': 'second', 'nodes': [
        {'id': 10, 'name': '黑诞', 'type': 'incident'}, {'id': 20, 'name': '黑诞', 'type': 'incident'}]}
    with zipfile.ZipFile(source, 'w') as z:
        z.writestr('run-fixture/manifest.json', '{}')
        z.writestr('run-fixture/run-events.jsonl', '\n'.join(json.dumps(e, ensure_ascii=False) for e in [left, right]) + '\n')
        z.writestr('run-fixture/routing-history.json', json.dumps(nodes, ensure_ascii=False))
        z.writestr('run-fixture/replay-data.js', 'const BLACKFLOW_RUN_EVENTS=[];\n' +
            ''.join('BLACKFLOW_RUN_EVENTS.push(' + json.dumps(e, ensure_ascii=False) + ');\n' for e in [left, right]))
        z.writestr('run-fixture/images/proof.jpg', b'exact original image bytes')
    plan = {'schema_version': 1, 'source_sha256': file_sha(source), 'path': '2026-09-05/123/28.zip',
        'reason': 'Reviewed aid page', 'author': 'fixture reviewer', 'corrections': [{'floor': 5,
        'transaction_id': 'second', 'node': 20, 'from': {'name': '黑诞', 'type': 'incident'},
        'to': {'name': '应急助力', 'type': 'employ'}, 'evidence': [
            {'path': 'images/proof.jpg', 'sha256': sha(b'exact original image bytes')}]}]}
    return source, plan


def test_corrected_files_images_original_and_provenance(tmp_path):
    source, plan = fixture(tmp_path)
    result = revise(source, tmp_path / 'result', plan)
    package = Path(result['package'])
    verified = verify_archive(package)
    assert verified['status'] == 'valid_curated_revision'
    assert verified['collector_signature_valid'] is False and verified['origin_attested'] is False
    assert file_sha(source) == plan['source_sha256']
    with zipfile.ZipFile(package) as z:
        events = [json.loads(e) for e in z.read('run-fixture/run-events.jsonl').splitlines()]
        assert events[0]['state']['page']['node_name'] == '黑诞'
        assert events[1]['state']['page']['node_type'] == 'employ'
        nodes = json.loads(z.read('run-fixture/routing-history.json'))['nodes']
        assert nodes[0]['name'] == '黑诞' and nodes[1]['name'] == '应急助力'
        assert z.read('run-fixture/images/proof.jpg') == b'exact original image bytes'
        assert z.read('run-fixture/replay-data.js').decode().count('应急助力') == 1
        assert json.loads(z.read('run-fixture/archive-revision.json'))['previous_sha256'] == plan['source_sha256']
    assert result['revision']['sha256'] == file_sha(package)


def test_stale_plan_or_unreviewed_evidence_rejected(tmp_path):
    source, plan = fixture(tmp_path)
    plan['source_sha256'] = '0' * 64
    with pytest.raises(ValueError, match='source SHA256'):
        revise(source, tmp_path / 'out', plan)
    plan['source_sha256'] = file_sha(source); plan['corrections'][0]['evidence'][0]['sha256'] = '0' * 64
    with pytest.raises(ValueError, match='evidence differs'):
        revise(source, tmp_path / 'out', plan)


def test_each_planned_correction_must_match_source_data(tmp_path):
    source, plan = fixture(tmp_path)
    missing = copy.deepcopy(plan['corrections'][0])
    missing['node'] = 999
    missing['transaction_id'] = 'never-visited'
    plan['corrections'].append(missing)
    with pytest.raises(ValueError, match='Every reviewed correction'):
        revise(source, tmp_path / 'out', plan)
    assert not list((tmp_path / 'out').glob('*.json'))


def test_combat_revision_preserves_other_maps_explicit_identity_and_historical_scores():
    correction = {'kind': 'combat_subtype', 'floor': 1, 'generations': [1, 3], 'node': 20,
        'transaction_id': 'tx', 'start_sequence': 10, 'from': {'type': 'battle_normal'},
        'to': {'type': 'hide_battle'}, 'reason': 'generic_combat_is_not_normal'}
    page = {'floor': 1, 'map_generation': 1, 'sequence': 10, 'transaction_id': 'tx',
        'state': {'page': {'node': 20, 'node_type': 'battle_normal', 'node_name': '作战'}},
        'details': {'candidate_comparison': [{'node': 20, 'node_type': 'battle_normal', 'score': 3}]}}
    fixed = correct_combat(page, correction)
    assert fixed['state']['page']['node_type'] == 'hide_battle'
    assert fixed['details'] == page['details']
    for field, other in [('floor', 2), ('map_generation', 2), ('transaction_id', 'other'), ('sequence', 9)]:
        different = copy.deepcopy(page); different[field] = other
        assert correct_combat(different, correction) == different
    note = {'floor': 1, 'map_generation': 3, 'nodes': [
        {'id': 20, 'type': 'battle_normal', 'identity_source': 'entered_page', 'progress': 'completed'},
        {'id': 20, 'type': 'battle_normal', 'identity_source': 'vision'},
        {'id': 21, 'type': 'battle_normal', 'identity_source': 'entered_page'}]}
    result = correct_combat(note, correction)
    assert result['nodes'][0]['type'] == 'hide_battle' and result['nodes'][0]['identity_unrecoverable']
    assert result['nodes'][1:] == note['nodes'][1:]
    assert correct_combat(result, correction) == result


def test_modified_derivative_is_not_accepted_as_unsigned_legacy(tmp_path):
    source, plan = fixture(tmp_path)
    result = revise(source, tmp_path / 'out', plan)
    broken = tmp_path / 'broken.zip'
    with zipfile.ZipFile(result['package']) as original, zipfile.ZipFile(broken, 'w') as z:
        for item in original.infolist():
            data = original.read(item)
            if item.filename.endswith('proof.jpg'): data = b'changed image'
            z.writestr(item, data)
        z.comment = original.comment
    with pytest.raises(ValueError, match='mismatch'):
        verify_archive(broken)
    with zipfile.ZipFile(tmp_path / 'future.zip', 'w') as z:
        z.comment = json.dumps({'format': 'maa-blackflow-curated-archive', 'schema_version': 2}).encode()
    with pytest.raises(UnsupportedContract):
        verify_archive(tmp_path / 'future.zip')


def test_map_return_correction_preserves_other_transactions_maps_and_decisions():
    correction = {'kind': 'landing_identity', 'floor': 4, 'generations': [6], 'node': 20,
        'transaction_id': 'tx', 'start_sequence': 10, 'from': {'type': 'incident', 'names': ['线人']},
        'to': {'type': 'door', 'name': '曲折密道'}, 'reason': 'map_labels_are_not_event_titles'}
    page = {'floor': 4, 'map_generation': 6, 'sequence': 10, 'transaction_id': 'tx',
        'state': {'page': {'node': 99, 'node_type': 'incident', 'node_name': '线人'}},
        'details': {'candidate_comparison': [{'node': 20, 'node_type': 'incident', 'score': 3}]}}
    result = correct_landing(page, correction)
    assert result['state']['page']['node_type'] == 'door' and result['state']['page']['node'] == 20
    assert result['details'] == page['details']
    for field, other in [('floor', 5), ('map_generation', 8), ('transaction_id', 'other'), ('sequence', 9)]:
        different = copy.deepcopy(page); different[field] = other
        assert correct_landing(different, correction) == different
    note = {'floor': 4, 'map_generation': 6, 'nodes': [
        {'id': 20, 'type': 'incident', 'name': '线人', 'identity_source': 'event_name', 'observed_contents': ['线人']},
        {'id': 99, 'type': 'incident', 'name': '线人', 'identity_source': 'event_name'}]}
    fixed = correct_landing(note, correction)
    assert fixed['nodes'][0]['name'] == '曲折密道' and fixed['nodes'][0]['observed_contents'] == []
    assert fixed['nodes'][1] == note['nodes'][1]
    assert correct_landing(fixed, correction) == fixed

    correction['restore_nodes'] = [{'node': 99, 'when': [
        {'identity_source': 'event_name', 'node_name': '线人', 'progress': 'completed'},
        {'identity_source': 'node_resolution_becomes_empty', 'node_type': 'empty'}],
        'fields': {'node_name': '不期而遇', 'node_type': 'incident', 'progress': 'active', 'blocks_vision': True}}]
    wrong = {'floor': 4, 'map_generation': 6, 'map_nodes': [
        {'id': 99, 'node_type': 'empty', 'node_name': '林间空地', 'progress': 'completed',
         'identity_source': 'node_resolution_becomes_empty', 'blocks_vision': False}]}
    recovered = correct_landing(wrong, correction)['map_nodes'][0]
    assert recovered['node_type'] == 'incident' and recovered['progress'] == 'active' and recovered['blocks_vision']
    wrong['map_nodes'][0]['identity_source'] = 'ocr'
    assert correct_landing(wrong, correction) == wrong
