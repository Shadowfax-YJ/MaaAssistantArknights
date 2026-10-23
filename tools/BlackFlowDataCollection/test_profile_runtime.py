import json
from pathlib import Path
import tempfile
import unittest

from profile_runtime import core_spans, paired_spans, partition, union_length


class ProfileRuntimeTests(unittest.TestCase):
    def test_nested_spans_have_one_wall_clock_denominator(self):
        spans = [{'start':0,'end':1000,'task':'AutomationShopDecision'},
                 {'start':100,'end':300,'task':'MovementInventorySwipeToStart'},
                 {'start':150,'end':200,'task':'MovementInventorySwipeToStart'}]
        result = partition(spans,1200)
        self.assertEqual(sum(result.values()),1200)
        self.assertEqual(result['商店'],800)
        self.assertEqual(result['零件箱回到起点'],200)

    def test_unfinished_callbacks_do_not_invent_completion(self):
        rows=[{'action':'process.X','task':'X','phase':'started','sequence':1,'elapsed_ms':0},
              {'action':'process.Y','task':'Y','phase':'started','sequence':2,'elapsed_ms':20},
              {'action':'process.Y','task':'Y','phase':'completed','sequence':3,'elapsed_ms':40}]
        spans=paired_spans(rows)
        self.assertEqual(len(spans),1)
        self.assertEqual(spans[0]['end']-spans[0]['start'],20)

    def test_unprefixed_fixed_task_callbacks_are_included(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'asst.log'
            payload={'subtask':'ProcessTask','details':{'task':'MovementInventorySwipeToStart','action':'Swipe'},
                     'first':['BlackFlow@Roguelike@MovementInventorySwipeToStart']}
            path.write_text('\n'.join('[2026-10-24 03:00:%s][INF][Px1][Tx2] Assistant::append_callback | %s %s' %
                (stamp,phase,json.dumps(payload)) for stamp,phase in [('00.000','SubTaskStart'),('01.300','SubTaskCompleted')]),encoding='utf-8')
            _,_,callbacks,windows,_=core_spans([path])
            self.assertEqual(len(callbacks),1)
            self.assertTrue(callbacks[0]['task'].startswith('BlackFlow@'))
            self.assertAlmostEqual(callbacks[0]['end_utc']-callbacks[0]['start_utc'],1.3,places=5)
            self.assertEqual(len(windows),1)

    def test_overlapping_and_missing_log_ranges_are_not_double_counted(self):
        self.assertEqual(union_length([(0,100),(20,40),(80,120),(200,250),(400,300)]),170)


if __name__ == '__main__':
    unittest.main()
