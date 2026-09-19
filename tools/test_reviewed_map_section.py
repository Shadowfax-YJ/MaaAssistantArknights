import copy
import hashlib
import json
import unittest

from BlackFlowReviewedMapSection import decode, validate
from ReviseBlackFlowRunArchive import transform


class ReviewedMapSectionTests(unittest.TestCase):
    def setUp(self):
        self.meta = {'floor': 4, 'map_generation': 7, 'map_section_generation': 5,
                     'floor_four_remembrance': False, 'map_section_key': 'floor-4-generation-5',
                     'map_section_label': '4 层', 'artifact_set_id': 'BF-A2-288',
                     'diagnostic_sequence': 288, 'nodes': [{'id': 42, 'type': 'incident'}]}
        self.c = {'kind': 'map_section_remembrance', 'floor': 4, 'map_generation': 7,
                  'map_section_generation': 5, 'run_revision': 2, 'start_sequence': 5361,
                  'artifact_ids': ['BF-A2-288'], 'approval_id': 'approved',
                  'review_ids': ['REM-1242'], 'evidence': [{'path': 'image.jpg'}]}

    def pin(self, name, raw, pointers):
        self.c['members'] = {name: {'source_sha256': hashlib.sha256(raw).hexdigest(),
                                   'locations': [{'json_path': p, 'artifact_set_id': 'BF-A2-288'} for p in pointers]}}
        validate(self.c)

    def test_json_and_both_history_scripts_preserve_graph_and_other_sections(self):
        value = [self.meta, {**self.meta, 'map_generation': 8}, {'snapshots': [self.meta]}]
        for name, prefix in [('routing-history.json', ''),
                             ('routing-history-data.js', 'const BLACKFLOW_ROUTING_HISTORY='),
                             ('processing-item-history-data.js', 'const BLACKFLOW_PROCESSING_ITEM_HISTORY=')]:
            raw = (prefix + json.dumps(value) + (';' if prefix else '')).encode()
            self.pin(name, raw, ['$/0', '$/2/snapshots/0'])
            fixed = decode(name, transform('run-test/' + name, raw, [self.c]).decode())
            self.assertTrue(fixed[0]['floor_four_remembrance'])
            self.assertEqual(fixed[0]['map_section_key'], 'floor-4-generation-5-remembrance')
            self.assertEqual(fixed[0]['nodes'], value[0]['nodes'])
            self.assertEqual(fixed[1], value[1])
            self.assertTrue(fixed[2]['snapshots'][0]['floor_four_remembrance'])
            self.assertFalse(value[0]['floor_four_remembrance'])

    def test_events_and_replay_preserve_callback_and_other_lines(self):
        event = {'sequence': 5361, 'run_revision': 2, 'task': 'NextLevel',
                 'details': {'artifact_set_id': 'BF-A2-288'}, 'state': self.meta}
        for name, raw in [('run-events.jsonl', json.dumps(event) + '\n'),
                          ('replay-data.js', 'const unrelated = 1;\nBLACKFLOW_RUN_EVENTS.push(' + json.dumps(event) + ');\n')]:
            raw = raw.encode(); self.pin(name, raw, ['$/0/state'])
            result = transform('run-test/' + name, raw, [self.c])
            fixed = decode(name, result.decode())[0]
            self.assertTrue(fixed['state']['floor_four_remembrance'])
            self.assertEqual(fixed['details'], event['details'])
            self.assertEqual(fixed['task'], event['task'])
            if name.endswith('.js'):
                self.assertTrue(result.startswith(b'const unrelated = 1;\n'))

    def test_unlisted_members_are_byte_identical(self):
        raw = json.dumps(self.meta).encode()
        self.pin('floor-4/BF-A2-288.snapshot.json', raw, ['$'])
        self.assertEqual(transform('run-test/other.json', raw, [self.c]), raw)

    def test_changed_preimage_and_reapplying_are_rejected(self):
        raw = json.dumps(self.meta).encode(); name = 'floor-4/BF-A2-288.snapshot.json'
        self.pin(name, raw, ['$'])
        fixed = transform('run-test/' + name, raw, [self.c])
        for bad in (raw + b' ', fixed):
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                transform('run-test/' + name, bad, [self.c])

    def test_wrong_map_time_or_artifact_rejected_even_with_matching_hash(self):
        for change in ({'floor': 6}, {'map_generation': 8}, {'map_section_generation': 6},
                       {'artifact_set_id': 'BF-A2-289'}, {'diagnostic_sequence': 289},
                       {'floor_four_remembrance': True}):
            raw = json.dumps({**self.meta, **change}).encode()
            name = 'floor-4/BF-A2-288.snapshot.json'; self.pin(name, raw, ['$'])
            with self.assertRaises(ValueError): transform('run-test/' + name, raw, [self.c])
        for change in ({'sequence': 5360}, {'run_revision': 3}, {'details': {'artifact_set_id': 'BF-A2-287'}}):
            event = {'state': self.meta, 'sequence': 5361, 'run_revision': 2,
                     'details': {'artifact_set_id': 'BF-A2-288'}, **change}
            raw = json.dumps(event).encode(); self.pin('run-events.jsonl', raw, ['$/0/state'])
            with self.assertRaises(ValueError): transform('run-test/run-events.jsonl', raw, [self.c])

    def test_missing_and_duplicate_locations_rejected(self):
        raw = json.dumps(self.meta).encode(); name = 'floor-4/BF-A2-288.snapshot.json'
        self.pin(name, raw, ['$/missing'])
        with self.assertRaises(ValueError): transform('run-test/' + name, raw, [self.c])
        with self.assertRaises(ValueError): self.pin(name, raw, ['$', '$'])

    def test_member_and_run_revision_boundaries(self):
        raw = json.dumps(self.meta).encode()
        for name in ('../routing-history.json', 'manifest.json', 'floor-5/BF-A2-288.snapshot.json'):
            with self.assertRaises(ValueError): self.pin(name, raw, ['$'])
        self.pin('floor-4/BF-A2-288.snapshot.json', raw, ['$'])
        wrong = copy.deepcopy(self.c); wrong['run_revision'] = 3
        with self.assertRaises(ValueError): validate(wrong)


if __name__ == '__main__':
    unittest.main()
