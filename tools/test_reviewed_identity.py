import copy
import json
import unittest

from BlackFlowReviewedIdentity import correct, validate, PREDICTION
from ReviseBlackFlowRunArchive import transform


class ReviewedIdentityTests(unittest.TestCase):
    def setUp(self):
        self.c = {'kind': 'notebook_identity', 'floor': 2, 'run_revision': 1, 'generations': [2],
                  'node': 42, 'transaction_id': 'T1', 'start_sequence': 100, 'end_sequence': 200,
                  'artifact_sequences': {'A-before': 99, 'A-after': 101, 'A-restored': 200},
                  'review_ids': ['BF-218'], 'approval_id': 'approved', 'reason': 'retain node intel',
                  'when': [{'node_type': 'battle_normal', 'stage_name': ''}],
                  'fields': {'stage_name': '虫虫游戏厅', 'name': '虫虫游戏厅', 'node_name': '虫虫游戏厅',
                             'identity_source': 'move_preview_stage_name'}}
        self.node = {'id': 42, 'type': 'battle_normal', 'node_type': 'battle_normal', 'stage_name': '',
                     'name': '作战', 'node_name': '作战', 'identity_source': 'entered_page', 'progress': 'completed',
                     'battle': {'stage_name': '强买强卖', 'total_kills': 10}}

    def frame(self, **kwargs):
        return {'floor': 2, 'run_revision': 1, 'map_generation': 2, 'sequence': 101,
                'map_nodes': [copy.deepcopy(self.node)], 'exploration_note_nodes': [copy.deepcopy(self.node)],
                'candidate_comparison': [{'node': copy.deepcopy(self.node), 'score': 8}], **kwargs}

    def test_only_lost_notebook_intel_changes(self):
        raw = self.frame(); frozen = copy.deepcopy(raw); fixed = correct(raw, self.c)
        note = fixed['exploration_note_nodes'][0]
        self.assertEqual(note['stage_name'], '虫虫游戏厅')
        self.assertEqual(note['battle'], self.node['battle'])
        self.assertEqual(note['progress'], 'completed')
        self.assertEqual(fixed['map_nodes'], raw['map_nodes'])
        self.assertEqual(fixed['candidate_comparison'], raw['candidate_comparison'])
        self.assertEqual(raw, frozen)
        self.assertEqual(correct(fixed, self.c), fixed)

    def test_scope_and_snapshot_time_are_strict(self):
        for extra in ({'floor': 3}, {'run_revision': 2}, {'map_generation': 3}, {'sequence': 99},
                      {'sequence': 200}, {'artifact_set_id': 'unknown'}, {'artifact_set_id': 'A-before'},
                      {'artifact_set_id': 'A-restored'}):
            raw = self.frame(**extra)
            self.assertEqual(correct(raw, self.c), raw, extra)
        raw = self.frame(artifact_set_id='A-after'); raw.pop('sequence')
        self.assertNotEqual(correct(raw, self.c), raw)
        raw['exploration_note_nodes'][0]['id'] = 43
        self.assertEqual(correct(raw, self.c), raw)

    def test_known_event_replaces_prediction_without_changing_battle(self):
        self.c.update(when=[{'identity_source': 'ideal_source_emergency_prediction'}],
                      fields={'node_type': 'incident', 'type': 'incident', 'name': '好奇心之死',
                              'node_name': '好奇心之死', 'fate_event': True, 'identity_from_prediction': False,
                              'prediction_rule': '', 'identity_revealed': True, 'identity_state': 'classified'})
        raw = self.frame(); raw['exploration_note_nodes'][0].update(PREDICTION, node_type='battle_elite')
        fixed = correct(raw, self.c)
        self.assertTrue(fixed['exploration_note_nodes'][0]['fate_event'])
        self.assertEqual(fixed['exploration_note_nodes'][0]['battle'], self.node['battle'])
        self.assertEqual(fixed['map_nodes'], raw['map_nodes'])

    def test_inferred_elite_supersedes_hidden_correction_but_not_cleared_map(self):
        self.c.update(kind='ideal_source_combat', floor=1, generations=[1], end_sequence=None,
                      to={'type':'battle_elite'}, center_evidence=[{'sequence': 90}])
        raw = self.frame(floor=1, map_generation=1, transaction_id='T1')
        note = raw['exploration_note_nodes'][0]
        note.update(node_type='hide_battle', type='hide_battle', identity_source='archive_revision',
                    identity_unrecoverable=True, node_type_correction={'method':'old'})
        note['battle']['stage_name'] = '急不可耐'
        raw['map_nodes'][0].update(node_type='empty', type='empty', identity_source='node_resolution_becomes_empty')
        raw['state'] = {'page': {'node': 42, 'node_type': 'hide_battle', 'node_name': '未知的凶戾'}}
        fixed = correct(raw, self.c); n = fixed['exploration_note_nodes'][0]
        self.assertEqual(n['node_type'], 'battle_elite')
        self.assertEqual(n['stage_name'], '急不可耐')
        self.assertTrue(n['identity_from_prediction']); self.assertFalse(n['identity_revealed'])
        self.assertFalse(n['identity_unrecoverable'])
        self.assertEqual(n['node_type_correction']['previous_correction'], {'method':'old'})
        self.assertEqual(fixed['map_nodes'], raw['map_nodes'])
        self.assertEqual(fixed['state']['page']['node_type'], 'battle_elite')
        self.assertEqual(correct(fixed, self.c), fixed)

    def test_replay_json_and_routing_have_identical_semantics(self):
        value=self.frame(artifact_set_id='A-after'); value.pop('sequence')
        target=correct(value,self.c)
        formats=[('run/x.json',json.dumps(value)),('run/run-events.jsonl',json.dumps(value)+'\n'),
                 ('run/routing-history-data.js','const BLACKFLOW_ROUTING_HISTORY='+json.dumps(value)+';'),
                 ('run/replay-data.js','BLACKFLOW_RUN_EVENTS.push('+json.dumps(value)+');')]
        for name,raw in formats:
            fixed=transform(name,raw.encode(),[self.c])
            self.assertIn('虫虫游戏厅',fixed.decode())
            self.assertEqual(transform(name,fixed,[self.c]),fixed)
        self.assertEqual(json.loads(transform('run/x.json',json.dumps(value).encode(),[self.c])),target)

    def test_reject_unreviewed_or_non_identity_fields(self):
        validate(self.c)
        invalid=copy.deepcopy(self.c); invalid['fields']['battle']={}
        with self.assertRaises(ValueError): validate(invalid)
        invalid=copy.deepcopy(self.c); invalid.pop('approval_id')
        with self.assertRaises(ValueError): validate(invalid)


if __name__ == '__main__':
    unittest.main()
