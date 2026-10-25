"""Collection OCR policy changes preserve raw archive/image contracts."""
import hashlib
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'unit_test/MaaCore/fixtures/blackflow-item-names/20261025'


def test_original_inventory_images_match_recorded_sources():
    sources = json.loads((FIXTURES / 'sources.json').read_bytes())
    assert len(sources) == 2
    for source in sources:
        assert hashlib.sha256((FIXTURES / source['file']).read_bytes()).hexdigest() == source['sha256']
        assert source['collector_version'] == 'v1.1.22'


def test_shared_item_names_agree_with_independent_analysis_catalogue():
    analysis = os.environ.get('BLACKFLOW_ANALYSIS_REPO')
    if not analysis:
        pytest.skip('BLACKFLOW_ANALYSIS_REPO is required')
    assert (Path(analysis) / 'tests/fixtures/blackflow-short-item-names.json').read_bytes() == (FIXTURES / 'matching-cases.json').read_bytes()
    sys.path.insert(0, str(Path(analysis) / 'src'))
    from lubiao_pipeline.catalog import resolve_text
    for cases in json.loads((FIXTURES / 'matching-cases.json').read_bytes()).values():
        for case in cases:
            item, _ = resolve_text(case['raw'])
            assert (item['name'] if item else None) == case['name']
