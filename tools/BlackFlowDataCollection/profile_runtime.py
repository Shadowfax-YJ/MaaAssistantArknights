"""Read-only wall-clock profiling from BlackFlow event streams and retained Core traces.

Intervals are unioned before attribution. Nested process/function spans must never
be summed into a run total. Recognition time is separately measured, not added again.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import statistics
import zipfile


def load_events(path):
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            member = next(n for n in archive.namelist() if n.endswith('/run-events.jsonl'))
            raw = archive.read(member)
    else:
        # A live file can have a partially written final line; snapshot its current prefix.
        raw = (path / 'run-events.jsonl').read_bytes()
    events = []
    for line in raw.splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            if line != raw.splitlines()[-1]:
                raise
    return events, hashlib.sha256(raw).hexdigest()


def paired_spans(events):
    pending = defaultdict(list)
    spans = []
    for event in events:
        if not event['action'].startswith('process.'):
            continue
        key = event.get('thread_id'), event['task']
        if event['phase'] == 'started':
            pending[key].append(event)
        elif event['phase'] == 'completed' and pending[key]:
            start = pending[key].pop()
            spans.append({'start': start['elapsed_ms'], 'end': event['elapsed_ms'],
                          'task': event['task'], 'first_sequence': start['sequence'],
                          'last_sequence': event['sequence'], 'source': 'process_callbacks'})
    return spans


def category(task):
    if any(word in task for word in ('RecoveryRetry', 'RecoveryFailed')):
        return '恢复与重试等待'
    if 'Inventory' in task:
        if 'SwipeToStart' in task or 'Reset' in task:
            return '零件箱回到起点'
        return '零件箱扫描与操作'
    if any(word in task for word in ('AutomationShop', 'AutomationCultivate', 'StageTrader', 'Cultivate')):
        return '商店'
    if any(word in task for word in ('Battle', 'Combat', 'StartButtonClicked', 'QuickFormation', 'StageDrops')):
        return '作战与编队'
    if any(word in task for word in ('Recruit', 'Recruitment', 'ChooseOper')):
        return '招募'
    if any(word in task for word in ('StageEncounter', 'Sacrifice', 'Expedition')):
        return '事件与派遣'
    if any(word in task for word in ('Map', 'Routing', 'Movement', 'MovePreview')):
        return '地图、规划与移动'
    if any(word in task for word in ('GetDrop', 'CloseCollection', 'LastReward', 'Opening')):
        return '奖励与开局'
    return '其他流程'


def partition(spans, duration):
    """Attribute each millisecond to the narrowest active interval; account for gaps."""
    points = defaultdict(list)
    for i, span in enumerate(spans):
        start, end = max(0, span['start']), min(duration, span['end'])
        if end <= start:
            continue
        points[start].append((True, i))
        points[end].append((False, i))
    points[0]; points[duration]
    active = set()
    totals = Counter()
    previous = 0
    for instant in sorted(points):
        if instant > previous:
            if active:
                selected = min(active, key=lambda i: (spans[i]['end'] - spans[i]['start'], -spans[i]['start']))
                bucket = category(spans[selected]['task'])
            else:
                bucket = '未覆盖间隔（识别、截图、等待等）'
            totals[bucket] += instant - previous
        for add, index in points[instant]:
            if add:
                active.add(index)
            else:
                active.discard(index)
        previous = instant
    assert sum(totals.values()) == duration
    return dict(totals)


def summarize(path):
    events, fingerprint = load_events(path)
    if not events:
        return None
    duration = events[-1]['elapsed_ms']
    spans = paired_spans(events)
    grouped = defaultdict(list)
    for span in spans:
        grouped[span['task']].append(span['end'] - span['start'])
    timings = sorted(({'task': task, 'count': len(values), 'total_ms': sum(values),
                       'median_ms': statistics.median(values), 'max_ms': max(values)}
                      for task, values in grouped.items()), key=lambda row: row['total_ms'], reverse=True)
    retries = [event for event in events if event['action'] == 'recovery.retry_scheduled']
    close_retries = [event for event in retries if 'MovementInventoryCloseTransitionWait' in event['details'].get('task', '')]
    fault_start = None
    if len(close_retries) >= 3:
        first = close_retries[0]['elapsed_ms']
        fault_start = max((span['start'] for span in spans
                           if span['task'].endswith('MovementInventoryClose') and span['start'] < first), default=first)
    inventory = [span for span in spans if span['task'].endswith('MovementInventoryObserve')]
    reset_swipes = [span for span in spans if span['task'].endswith('MovementInventorySwipeToStart')]
    resets = []
    for scan in inventory:
        swipes = [span for span in reset_swipes if scan['start'] <= span['start'] < scan['end']]
        resets.append({'start_ms': scan['start'], 'duration_ms': scan['end'] - scan['start'],
                       'left_swipes': len(swipes), 'swipe_action_ms': sum(s['end'] - s['start'] for s in swipes),
                       'first_sequence': scan['first_sequence']})
    return {'instance': path.parent.parent.parent.name if path.suffix == '.zip' else path.parent.parent.parent.name,
            'run_id': path.stem if path.suffix == '.zip' else path.name, 'source': str(path),
            'source_sha256': fingerprint, 'events': len(events), 'start_utc': events[0]['timestamp'],
            'end_utc': events[-1]['timestamp'], 'duration_ms': duration, 'last_action': events[-1]['action'],
            'last_outcome': events[-1].get('outcome'), 'collector_version': events[0].get('collector_version'),
            'run_end_actions': [e['action'] + ':' + e.get('outcome', '') for e in events[-8:]],
            'fault_start_ms': fault_start, 'fault_tail_ms': duration - fault_start if fault_start is not None else 0,
            'close_retries': len(close_retries), 'retry_count': len(retries),
            'categories_ms': partition(spans, duration), 'tasks': timings[:35], 'inventory_scans': resets,
            '_start': datetime.fromisoformat(events[0]['timestamp'].replace('Z', '+00:00')).timestamp(),
            '_spans': spans, '_events': events}


STAMP = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*?\[T([^]]+)\] (.*)$')
TRACE = re.compile(r'(.+?) \| (enter|leave)(?:, (\d+) ms)?$')


def text_lines(path):
    with path.open(encoding='utf-8', errors='replace') as stream:
        yield from stream


def core_spans(logs):
    starts = defaultdict(list)
    spans, ocr, callbacks, windows, merchant_rechecks = [], [], [], [], []
    callback_starts = defaultdict(list)
    # A reset swipe sequence is measured from its first enter to the last leave.
    for path in logs:
        first_time = last_time = None
        for line in text_lines(path):
            if not any(word in line for word in ('| enter', '| leave', 'cost ', 'BlackFlow automation', 'BlackFlow inventory',
                                                 'to_be_recognized', 'Assistant::append_callback | SubTaskStart',
                                                 'Assistant::append_callback | SubTaskCompleted')):
                continue
            stamp = STAMP.match(line.rstrip())
            if not stamp:
                continue
            when = datetime.fromisoformat(stamp[1]).replace(tzinfo=timezone.utc).timestamp() - 8*3600
            first_time = when if first_time is None else min(first_time, when)
            last_time = when if last_time is None else max(last_time, when)
            thread, body = stamp[2], stamp[3]
            if '坐标校验失败' in body:
                merchant_rechecks.append({'timestamp_utc':when,'detail':body})
            callback = re.search(r'Assistant::append_callback \| (SubTaskStart|SubTaskCompleted) (\{.*\})$', body)
            if callback:
                payload = json.loads(callback[2])
                details = payload.get('details') or {}
                task = details.get('task')
                if task and payload.get('subtask') == 'ProcessTask':
                    key = thread, task
                    if callback[1] == 'SubTaskStart':
                        callback_starts[key].append((when, details, payload.get('first') or []))
                    elif callback_starts[key]:
                        start, info, first = callback_starts[key].pop()
                        if first and first[0].endswith(task):
                            task = first[0]
                        elif any('BlackFlow@' in name for name in first) and not task.startswith('BlackFlow@'):
                            task = 'BlackFlow@Roguelike@' + task
                        callbacks.append({'start_utc': start, 'end_utc': when, 'task': task,
                                          'action': info.get('action'), 'source': 'core_callbacks'})
            trace = TRACE.fullmatch(body)
            if trace:
                name, phase = trace[1], trace[2]
                key = thread, name
                if phase == 'enter':
                    starts[key].append(when)
                elif starts[key]:
                    start = starts[key].pop()
                    if when >= start:
                        spans.append({'start_utc': start, 'end_utc': when, 'function': name, 'thread': thread})
            if 'cost ' in body:
                cost = re.search(r'by (OCR.*?) , cost (\d+) ms', body)
                if cost:
                    ocr.append((when, int(cost[2])))
        if first_time is not None:
            windows.append({'source':str(path),'start_utc':first_time,'end_utc':last_time})
    return spans, ocr, callbacks, windows, merchant_rechecks


def union_length(intervals):
    total, end = 0, None
    for first, last in sorted(intervals):
        if last <= first:
            continue
        total += max(0, last - max(first, end if end is not None else first))
        end = max(last, end if end is not None else last)
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--instances', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--per-instance', type=int, default=2)
    parser.add_argument('--date', default='20261024')
    args = parser.parse_args()
    runs = []
    for instance in ('MAA-001', 'MAA-002', 'MAA-003'):
        debug = args.instances / instance / 'debug'
        paths = sorted((debug / 'BlackFlow').glob('run-' + args.date + '*'), reverse=True)
        chosen = [p for p in paths if p.suffix == '.zip'][:args.per_instance]
        # Keep failures and in-progress snapshots separate from completed archives.
        chosen += [p for p in paths if p.is_dir() and (p / 'run-events.jsonl').exists()]
        logs = sorted(debug.glob('asst*.log'), key=lambda path: path.stat().st_mtime)
        functions, ocr, callbacks, windows, merchant_rechecks = core_spans(logs)
        for path in chosen:
            result = summarize(path)
            if result is None:
                continue
            result['instance'] = instance
            start = result.pop('_start')
            end = start + result['duration_ms']/1000
            coverage = union_length((max(start,w['start_utc']),min(end,w['end_utc'])) for w in windows)
            result['core_log_coverage_ratio'] = min(1,coverage*1000/result['duration_ms'])
            result['core_logs'] = windows
            matching = [span for span in functions if start <= span['start_utc'] <= span['end_utc'] <= end]
            core = [{**span, 'start': round((span['start_utc'] - start)*1000),
                     'end': round((span['end_utc']-start)*1000)} for span in callbacks
                    if start <= span['start_utc'] <= span['end_utc'] <= end]
            function_tasks = {'RoguelikeBattleTaskPlugin::_run': 'BattleTaskPlugin',
                              'RoguelikeRecruitTaskPlugin::_run': 'RecruitTaskPlugin',
                              'RoguelikeStageEncounterTaskPlugin::_run': 'StageEncounterTaskPlugin',
                              'BlackFlowAutomationStoreTaskPlugin::_run': 'AutomationShopPlugin',
                              'BlackFlowRoutingTaskPlugin::_run': 'RoutingPlugin'}
            high_level = [{**span, 'start': round((span['start_utc']-start)*1000),
                           'end': round((span['end_utc']-start)*1000), 'task': task}
                          for span in matching for suffix, task in function_tasks.items()
                          if span['function'].endswith(suffix)]
            result['callback_categories_ms'] = result['categories_ms']
            result['categories_ms'] = partition(result['_spans'] + core + high_level, result['duration_ms'])
            for scan in result['inventory_scans']:
                inside = [s for s in core if scan['start_ms']-5 <= s['start'] < scan['start_ms'] + scan['duration_ms']]
                swipes = [s for s in inside if s['task'].endswith('MovementInventorySwipeToStart')]
                scan['core_log_observed'] = bool(inside)
                scan['left_swipes'] = len(swipes) if inside else None
                scan['swipe_action_ms'] = sum(s['end']-s['start'] for s in swipes) if inside else None
                scan['reset_span_ms'] = max(s['end'] for s in swipes) - min(s['start'] for s in swipes) if swipes else None
            result['core_tasks'] = []
            by_task = defaultdict(list)
            for span in core:
                by_task[span['task']].append(span['end']-span['start'])
            result['core_tasks'] = sorted(({'task':name,'count':len(times),'total_ms':sum(times),'median_ms':statistics.median(times),'max_ms':max(times)}
                for name,times in by_task.items()),key=lambda row:row['total_ms'],reverse=True)[:45]
            # Measured shop entry latency, which includes passive inventory scans and image capture.
            enters = [s for s in result['_spans'] if s['task'].endswith(('AutomationShopEnter','AutomationCultivateEnter'))]
            result['store_entries'] = []
            for entry in enters:
                next_entry = min((s['start'] for s in enters if s['start'] > entry['start']),default=result['duration_ms'])
                inputs = [s for s in core if entry['end'] <= s['start'] < next_entry and s.get('action') not in (None,'DoNothing')
                          and any(word in s['task'] for word in ('Shop','Cultivate'))]
                actions = [e for e in result['_events'] if entry['start'] < e['elapsed_ms'] < next_entry
                           and e['action'].startswith(('store.eerie.','store.secret.')) and e['action'].endswith('.select')]
                result['store_entries'].append({'start_ms':entry['start'],'kind':'eerie' if entry['task'].endswith('AutomationShopEnter') else 'secret',
                    'first_logged_input_ms': min((s['start'] for s in inputs),default=0)-entry['start'] if inputs else None,
                    'first_selection_ms': actions[0]['elapsed_ms']-entry['start'] if actions else None,
                    'first_selection_action':actions[0]['action'] if actions else None,
                    'position_rechecks_before_first_selection':[
                        {**warning,'elapsed_ms':round((warning['timestamp_utc']-start)*1000)}
                        for warning in merchant_rechecks
                        if start+entry['start']/1000 <= warning['timestamp_utc'] < start+(actions[0]['elapsed_ms'] if actions else next_entry)/1000]})
            values = defaultdict(list)
            for span in matching:
                values[span['function']].append(round((span['end_utc']-span['start_utc'])*1000))
            result['functions_inclusive'] = sorted(({'function': name, 'count': len(times), 'total_ms': sum(times),
                'median_ms': statistics.median(times), 'max_ms': max(times)} for name,times in values.items()),
                key=lambda row:row['total_ms'],reverse=True)[:35]
            calls = [cost for when,cost in ocr if start <= when <= end]
            result['ocr_calls_in_retained_logs'] = len(calls)
            result['ocr_ms_in_retained_logs'] = sum(calls)
            result['core_log_coverage_ms'] = round(coverage*1000)
            result.pop('_spans')
            result.pop('_events')
            runs.append(result)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'created_at': datetime.now(timezone.utc).isoformat(), 'method': 'paired callbacks, narrowest interval partition; Core function times inclusive and non-additive',
              'limits': ['No CPU sampler: wall-clock != CPU time.', 'Retained Core logs can cover only part of an older run.',
                         'Incomplete/stopped runs are separate from completed archives.', 'Stability probes and OCR overlap task spans; never sum them again.'],
              'runs': runs}
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for row in runs:
        print(row['instance'],row['run_id'],round(row['duration_ms']/60000,2),'min',row['last_action'],row['last_outcome'],
              'scans',len(row['inventory_scans']),'observed reset swipes',sum(s['left_swipes'] or 0 for s in row['inventory_scans']),
              'close retries',row['close_retries'])


if __name__ == '__main__':
    main()
