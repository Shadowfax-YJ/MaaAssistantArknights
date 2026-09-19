"""Materialize an approved remembrance return segment at pinned JSON locations.

This is a historical correction, not a general JSON patch interface. Only the
three section identity fields can change, in explicitly hashed source members.
"""
import copy
import hashlib
import json
from pathlib import PurePosixPath
import re

KIND = 'map_section_remembrance'
FIELDS = ('floor_four_remembrance', 'map_section_key', 'map_section_label')
HISTORIES = {
    'routing-history-data.js': 'const BLACKFLOW_ROUTING_HISTORY=',
    'processing-item-history-data.js': 'const BLACKFLOW_PROCESSING_ITEM_HISTORY=',
}
EVENT_PREFIX = 'BLACKFLOW_RUN_EVENTS.push('


def validate(c):
    if (c.get('kind') != KIND or c.get('floor') != 4
            or type(c.get('map_generation')) is not int or c['map_generation'] < 1
            or type(c.get('map_section_generation')) is not int or c['map_section_generation'] < 1
            or type(c.get('run_revision')) is not int or c['run_revision'] < 1
            or not c.get('start_sequence') or not c.get('artifact_ids')
            or not c.get('review_ids') or not c.get('approval_id') or not c.get('evidence')
            or not c.get('members')):
        raise ValueError('Map section repair requires reviewed physical, temporal and member scope')
    for artifact in c['artifact_ids']:
        if not re.fullmatch(r'BF-A' + str(c['run_revision']) + r'-[1-9][0-9]*', artifact):
            raise ValueError('Map section artifact does not belong to the reviewed run revision')
    for name, member in c['members'].items():
        path = PurePosixPath(name)
        allowed = (name in ('run-events.jsonl', 'replay-data.js', 'routing-history.json',
                           'processing-item-history.json', *HISTORIES)
                   or re.fullmatch(r'floor-4/BF-A[0-9]+-[0-9]+\.snapshot\.json', name))
        if (not allowed or path.is_absolute() or '..' in path.parts
                or not re.fullmatch('[0-9a-f]{64}', member.get('source_sha256', ''))
                or not member.get('locations')):
            raise ValueError('Unsupported or unpinned map section member')
        pointers = [loc.get('json_path') for loc in member['locations']]
        if len(set(pointers)) != len(pointers) or any(not isinstance(p, str) or not p.startswith('$') for p in pointers):
            raise ValueError('Invalid or duplicate map section location')


def decode(name, text):
    if name == 'run-events.jsonl':
        return [json.loads(line) for line in text.splitlines()]
    if name == 'replay-data.js':
        return [json.loads(line[len(EVENT_PREFIX):-2]) for line in text.splitlines()
                if line.startswith(EVENT_PREFIX) and line.endswith(');')]
    if name in HISTORIES:
        prefix = HISTORIES[name]
        if not text.startswith(prefix):
            raise ValueError('Unsupported map section history script')
        return json.loads(text[len(prefix):].strip().removesuffix(';'))
    return json.loads(text)


def transform(name, data, c):
    relative = name.partition('/')[2]
    member = c['members'].get(relative)
    if member is None:
        return data
    if hashlib.sha256(data).hexdigest() != member['source_sha256']:
        raise ValueError('Reviewed map section member SHA256 differs from source')
    text = data.decode('utf-8-sig')
    original = decode(relative, text)
    value = copy.deepcopy(original)
    generation = c['map_section_generation']
    expected = {'floor': 4, 'map_generation': c['map_generation'],
                'map_section_generation': generation, 'floor_four_remembrance': False,
                'map_section_key': f'floor-4-generation-{generation}', 'map_section_label': '4 层'}
    for location in member['locations']:
        pointer = location['json_path']
        if pointer != '$' and not pointer.startswith('$/'):
            raise ValueError('Invalid map section JSON location')
        target = value
        parts = pointer.split('/')[1:]
        try:
            for part in parts:
                target = target[int(part)] if isinstance(target, list) else target[part]
        except (KeyError, IndexError, ValueError, TypeError) as error:
            raise ValueError('Reviewed map section location is missing') from error
        if not isinstance(target, dict) or any(target.get(k) != v for k, v in expected.items()):
            raise ValueError('Map section location no longer has the reviewed identity')
        if relative in ('run-events.jsonl', 'replay-data.js'):
            if len(parts) != 2 or parts[1] != 'state':
                raise ValueError('Only event state section metadata may be repaired')
            event = value[int(parts[0])]
            if (event.get('run_revision') != c['run_revision']
                    or event.get('sequence', 0) < c['start_sequence']
                    or event.get('details', {}).get('artifact_set_id') not in c['artifact_ids']):
                raise ValueError('Event is outside the reviewed return segment')
        else:
            artifact = target.get('artifact_set_id')
            if (artifact not in c['artifact_ids'] or artifact != location.get('artifact_set_id')
                    or target.get('diagnostic_sequence') != int(artifact.rsplit('-', 1)[1])):
                raise ValueError('Snapshot is outside the reviewed return segment')
        target.update(floor_four_remembrance=True,
                      map_section_key=f'floor-4-generation-{generation}-remembrance',
                      map_section_label='追忆 4 层')
    def encode(item):
        return json.dumps(item, ensure_ascii=False, separators=(',', ':'))
    if relative == 'run-events.jsonl':
        return ('\n'.join(encode(item) if item != before else line
                          for line, before, item in zip(text.splitlines(), original, value)) + '\n').encode('utf-8')
    if relative == 'replay-data.js':
        lines, index = [], 0
        for line in text.splitlines():
            if line.startswith(EVENT_PREFIX) and line.endswith(');'):
                if original[index] != value[index]:
                    line = EVENT_PREFIX + encode(value[index]) + ');'
                index += 1
            lines.append(line)
        return ('\n'.join(lines) + '\n').encode('utf-8')
    prefix = HISTORIES.get(relative, '')
    return (prefix + encode(value) + (';\n' if prefix else '\n')).encode('utf-8')
