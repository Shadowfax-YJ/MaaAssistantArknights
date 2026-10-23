"""Shared real producer envelopes, independent signature verification and consumer."""
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'unit_test/MaaCore/fixtures/blackflow-encounter/20261024'


def test_actual_encounter_archives_accept_independent_v1_verifier():
    spec = importlib.util.spec_from_file_location('expedition_contract_helpers', ROOT / 'unit_test/MaaCore/test_blackflow_expedition_contract.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.FIXTURES = FIXTURE
    module.test_actual_node_images_and_all_lifecycle_cases_accept_v1_signed_envelope()


def test_shared_consumer_fixture_is_identical_and_choices_are_organized():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required')
    assert (Path(analysis) / 'tests/fixtures/blackflow-encounter-events.json').read_bytes() == (FIXTURE / 'producer-events.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.processing import plan
    for case in json.loads((FIXTURE / 'producer-events.json').read_bytes()):
        for event in case['events']:
            if event['action'].startswith('node.encounter.choice.'):
                task = plan(event)
                assert task['family'] == 'encounter_choice_evidence'
                assert task['recognizers'] == []
