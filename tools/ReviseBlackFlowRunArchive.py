"""Materialize an explicitly reviewed node correction; never overwrite the source.

The derivative has its own integrity profile. The collector's original signature
is retained as provenance, never reused to claim the edited bytes were collected.
"""
import argparse
import copy
import datetime as dt
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import uuid
import zipfile

from VerifyBlackFlowRunArchive import verify_archive, safe_name


def encode(value):
    return (json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def correct_combat(value, correction, floor=None, generation=None, transaction=None, sequence=None, container=''):
    """Change generic combat identities, retaining original decisions and revealed subtypes."""
    if isinstance(value, list):
        return [correct_combat(v, correction, floor, generation, transaction, sequence, container) for v in value]
    if not isinstance(value, dict):
        return value
    floor = value.get('floor', floor)
    generation = value.get('map_generation', generation)
    transaction = value.get('transaction_id') or transaction
    sequence = value.get('sequence', sequence)
    context = floor == correction['floor'] and generation in correction['generations']
    result = dict(value)
    keys = ('details', 'state', 'page', 'attribution', 'map', 'exploration_notebook', 'nodes',
            'map_nodes', 'exploration_note_nodes', 'snapshots', 'frames')
    for key in keys:
        if key in value:
            result[key] = correct_combat(value[key], correction, floor, generation, transaction, sequence, key)
    map_node = (value.get('id') == correction['node'] and value.get('identity_source') == 'entered_page')
    page = (transaction == correction['transaction_id'] and value.get('node', correction['node']) == correction['node']
            and (sequence is None or sequence >= correction['start_sequence'])
            and (container in ('page', 'attribution') or value.get('node') == correction['node']))
    if context and (map_node or page) and any(value.get(key) == correction['from']['type'] for key in ('type', 'node_type')):
        for key in ('type', 'node_type'):
            if value.get(key) == correction['from']['type']:
                result[key] = correction['to']['type']
        result['node_type_correction'] = {'method': 'archive-combat-identity-1', 'raw_type': correction['from']['type'],
            'transaction_id': correction['transaction_id'], 'reason': correction['reason']}
        if map_node:
            revealed = correction['to']['type'] != 'hide_battle'
            result.update(identity_source='archive_revision', identity_revealed=revealed, semantically_known=revealed,
                          identity_state='classified' if revealed else 'hidden')
            if not revealed and value.get('progress') == 'completed':
                result['identity_unrecoverable'] = True
        for key in ('name', 'node_name'):
            if value.get(key) == '作战' and correction['to']['type'] == 'hide_battle':
                result[key] = '未知的凶戾'
    return result


def correct_landing(value, correction, floor=None, generation=None, transaction=None, sequence=None, container=''):
    """Repair reviewed map-return contamination, without rewriting historical choices."""
    if isinstance(value, list):
        return [correct_landing(v, correction, floor, generation, transaction, sequence, container) for v in value]
    if not isinstance(value, dict):
        return value
    floor = value.get('floor', floor)
    generation = value.get('map_generation', generation)
    transaction = value.get('transaction_id') or transaction
    sequence = value.get('sequence', sequence)
    result = dict(value)
    for key in ('details', 'state', 'page', 'attribution', 'map', 'exploration_notebook', 'nodes',
                'map_nodes', 'exploration_note_nodes', 'snapshots', 'frames'):
        if key in value:
            result[key] = correct_landing(value[key], correction, floor, generation, transaction, sequence, key)
    if floor != correction['floor'] or generation not in correction['generations']:
        return result
    if sequence is not None and sequence < correction['start_sequence']:
        return result
    # A false page can bind to a different, still-unvisited semantic candidate.
    # Restore only the reviewed contamination states; retain later OCR/real visits.
    for restore in correction.get('restore_nodes', []):
        if value.get('id') != restore['node'] or not any(
                all(value.get(key) == expected for key, expected in match.items()) for match in restore['when']):
            continue
        for key, restored in restore['fields'].items():
            if key in value:
                result[key] = restored
        result['identity_source'] = 'archive_revision'
        result['node_type_correction'] = {'method': 'archive-map-return-1', 'raw_type': value.get('type', value.get('node_type')),
            'raw_name': value.get('name', value.get('node_name')), 'transaction_id': correction['transaction_id'],
            'reason': 'restore_unvisited_node_after_false_page_binding'}
        return result
    map_node = (value.get('id') == correction['node'] and value.get('identity_source') in ('entered_page', 'event_name'))
    page = transaction == correction['transaction_id'] and (container in ('page', 'attribution') or 'node' in value)
    warning = transaction == correction['transaction_id'] and value.get('actual_landing') == correction['node']
    if not (map_node or page or warning):
        return result
    if not any(value.get(key) == correction['from']['type'] for key in ('type', 'node_type', 'event_node_type')):
        return result
    for key in ('type', 'node_type', 'event_node_type'):
        if value.get(key) == correction['from']['type']:
            result[key] = correction['to']['type']
    for key in ('name', 'node_name', 'event_name'):
        if key in value:
            result[key] = correction['to']['name']
    if page and 'node' in value:
        result['node'] = correction['node']
    for key in ('observed_contents', 'observed_page_contents'):
        if key in value:
            result[key] = [s for s in value[key] if s not in correction['from']['names']]
    if map_node:
        result.update(identity_source='archive_revision', identity_revealed=True, semantically_known=True,
                      identity_state='classified', identity_unrecoverable=False)
    result['node_type_correction'] = {'method': 'archive-map-return-1', 'raw_type': correction['from']['type'],
        'raw_name': value.get('name', value.get('node_name', value.get('event_name'))),
        'transaction_id': correction['transaction_id'], 'reason': correction['reason']}
    return result


def correct_identity(value, correction, floor=None, transaction=None, bound_node=None):
    if correction.get('kind') == 'combat_subtype':
        return correct_combat(value, correction)
    if correction.get('kind') == 'landing_identity':
        return correct_landing(value, correction)
    if isinstance(value, list):
        return [correct_identity(v, correction, floor, transaction, bound_node) for v in value]
    if not isinstance(value, dict):
        return value
    floor = value.get('floor', floor)
    transaction = value.get('transaction_id') or transaction
    node = value.get('id', value.get('node', value.get('actual_landing', bound_node)))
    unknown = node in (None, 18446744073709551615)
    matches = floor == correction['floor'] and (node == correction['node'] or
        (unknown and transaction == correction['transaction_id']))
    result = {k: correct_identity(v, correction, floor, transaction, node) for k, v in value.items()}
    if matches:
        old, new = correction['from'], correction['to']
        named = any(value.get(k) == old['name'] for k in ('name', 'node_name', 'event_name'))
        if named:
            for key in ('name', 'node_name', 'event_name'):
                if value.get(key) == old['name']:
                    result[key] = new['name']
            for key in ('type', 'node_type', 'event_node_type'):
                if value.get(key) == old['type']:
                    result[key] = new['type']
            if 'identity_source' in value:
                result['identity_source'] = 'archive_revision'
        for key in ('observed_contents', 'observed_page_contents'):
            if isinstance(value.get(key), list):
                result[key] = [new['name'] if text == old['name'] else text for text in value[key]]
    return result


def transform(name, data, corrections):
    def apply(value):
        for correction in corrections:
            value = correct_identity(value, correction)
        return value
    text = data.decode('utf-8-sig')
    if name.endswith('.json'):
        value = json.loads(text)
        corrected = apply(value)
        return data if corrected == value else encode(corrected)
    if name.endswith('/run-events.jsonl'):
        lines, changed = [], False
        for line in text.splitlines():
            value = json.loads(line); corrected = apply(value)
            changed |= corrected != value
            lines.append(json.dumps(corrected, ensure_ascii=False, separators=(',', ':')))
        return ('\n'.join(lines) + '\n').encode() if changed else data
    if name.endswith('/routing-history-data.js'):
        prefix = 'const BLACKFLOW_ROUTING_HISTORY='
        if not text.startswith(prefix):
            raise ValueError('Unsupported routing history script')
        value = json.loads(text[len(prefix):].strip().removesuffix(';'))
        corrected = apply(value)
        return (prefix + encode(corrected).decode().strip() + ';\n').encode() if corrected != value else data
    if name.endswith('/replay-data.js'):
        prefix = 'BLACKFLOW_RUN_EVENTS.push('
        changed, lines = False, []
        for line in text.splitlines():
            if line.startswith(prefix) and line.endswith(');'):
                value = json.loads(line[len(prefix):-2]); corrected = apply(value)
                changed |= value != corrected
                line = prefix + encode(corrected).decode().strip() + ');'
            lines.append(line)
        return ('\n'.join(lines) + '\n').encode() if changed else data
    return data


def revise(source, output, plan):
    source, output = Path(source), Path(output)
    before = file_sha(source)
    if plan.get('schema_version') != 1 or plan.get('source_sha256') != before:
        raise ValueError('Reviewed source SHA256 does not match')
    relative = PurePosixPath(plan['path'])
    safe_name(plan['path'])
    if relative.parts[0].startswith('.') or not plan.get('reason') or not plan.get('author'):
        raise ValueError('Missing review author/reason or invalid archive path')
    validation = verify_archive(source)
    if validation['status'] not in ('unsigned_or_repacked', 'valid_local_signature', 'valid_curated_revision'):
        raise ValueError('Source validation did not pass')
    revision_id = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:12]
    created = dt.datetime.now(dt.timezone.utc).isoformat()
    output.mkdir(parents=True, exist_ok=True)
    package = output / (revision_id + '.zip')
    if package.resolve() == source.resolve():
        raise ValueError('Source archive cannot be overwritten')
    changes, inventory = [], []
    applied = [0 for _ in plan['corrections']]
    with zipfile.ZipFile(source) as original:
        roots = {PurePosixPath(n).parts[0] for n in original.namelist()}
        if len(roots) != 1 or not next(iter(roots)).startswith('run-'):
            raise ValueError('One complete run directory is required')
        root = next(iter(roots))
        seen = set()
        for c in plan['corrections']:
            if not c.get('evidence') or not c.get('transaction_id') or c.get('floor') not in range(1, 7):
                raise ValueError('Correction needs transaction, floor and reviewed evidence')
            if c.get('kind') == 'combat_subtype' and (not c.get('generations') or not c.get('start_sequence')
                    or c.get('from', {}).get('type') != 'battle_normal'
                    or c.get('to', {}).get('type') not in ('hide_battle', 'battle_elite', 'battle_savage', 'battle_boss')):
                raise ValueError('Combat correction requires scoped map generations and supported subtype')
            if c.get('kind') == 'landing_identity' and (not c.get('generations') or not c.get('start_sequence')
                    or not c.get('from', {}).get('names') or c.get('to', {}).get('type') not in ('empty', 'door')):
                raise ValueError('Map return correction requires scoped generations and reviewed landing identity')
            for proof in c['evidence']:
                if sha(original.read(root + '/' + proof['path'])) != proof['sha256']:
                    raise ValueError('Reviewed evidence differs from source')
        with zipfile.ZipFile(package, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
            for entry in original.infolist():
                safe_name(entry.filename.rstrip('/'))
                if entry.filename.casefold() in seen or entry.flag_bits & 1 or (entry.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('Unsafe or duplicate archive member')
                seen.add(entry.filename.casefold())
                if entry.is_dir():
                    continue
                data = original.read(entry)
                name = entry.filename
                if name == root + '/archive-revision.json' or name == root + '/integrity.json':
                    # Retain previous manifests as historical evidence, never as current integrity.
                    name = root + '/revision-provenance/' + before + '-' + PurePosixPath(name).name
                revised = data
                if '/revision-provenance/' not in name and name.endswith(('.json', '.jsonl', '.js')):
                    for index, correction in enumerate(plan['corrections']):
                        updated = transform(name, revised, [correction])
                        applied[index] += updated != revised
                        revised = updated
                if name != entry.filename or revised != data:
                    changes.append({'member': entry.filename, 'result_member': name,
                        'before_sha256': sha(data), 'after_sha256': sha(revised),
                        'before_size': len(data), 'after_size': len(revised)})
                copied = copy.copy(entry); copied.filename = name
                target.writestr(copied, revised)
                inventory.append({'path': name, 'sha256': sha(revised), 'size': len(revised)})
            if not applied or not all(applied):
                raise ValueError('Every reviewed correction must change matching source data')
            document = {'format': 'maa-blackflow-curated-archive', 'schema_version': 1,
                'revision_id': revision_id, 'created_at': created, 'author': plan['author'], 'reason': plan['reason'],
                'origin_attested': False, 'previous_sha256': before, 'previous_size': source.stat().st_size,
                'original_validation': validation, 'original_zip_comment_hex': original.comment.hex(),
                'corrections': plan['corrections'], 'changes': changes, 'files': inventory}
            data = encode(document); manifest = root + '/archive-revision.json'
            target.writestr(manifest, data)
            target.comment = encode({'format': document['format'], 'schema_version': 1,
                'origin_attested': False, 'manifest': manifest, 'manifest_sha256': sha(data)}).strip()
    if file_sha(source) != before:
        raise ValueError('Source changed while preparing revision')
    if verify_archive(package)['status'] != 'valid_curated_revision':
        raise ValueError('Revised archive failed independent verification')
    digest = file_sha(package)
    record = {'format': 'quark-file-revision', 'schema_version': 1, 'revision_id': revision_id,
        'created_at': created, 'author': plan['author'], 'reason': plan['reason'], 'path': plan['path'],
        'previous_sha256': before, 'sha256': digest, 'size': package.stat().st_size,
        'content_path': '.sync-revisions/objects/' + digest,
        'recycle_path': '.sync-recycle/' + before + '/' + plan['path'], 'changes': changes}
    record_path = output / (revision_id + '.json'); record_path.write_bytes(encode(record))
    return {'package': str(package), 'record': str(record_path), 'revision': record}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = revise(args.source, args.output, json.loads(args.plan.read_text(encoding='utf-8-sig')))
    print(json.dumps({k: v for k, v in result.items() if k != 'revision'}, ensure_ascii=False))
