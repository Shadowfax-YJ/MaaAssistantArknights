"""Narrow, explicitly reviewed historical notebook repairs.

These transformations never traverse route candidates, OCR evidence, or battle
payloads. Sequence bounds also apply to snapshots through their diagnostic IDs.
"""

KINDS = {'notebook_identity', 'ideal_source_combat'}
CONTAINERS = ('details', 'state', 'page', 'attribution', 'map', 'exploration_notebook',
              'nodes', 'map_nodes', 'exploration_note_nodes', 'snapshots', 'frames')
CONTEXT = ('floor', 'map_generation', 'run_revision', 'sequence', 'transaction_id')
PREDICTION = {
    'identity_source': 'ideal_source_emergency_prediction',
    'identity_from_prediction': True, 'prediction_rule': 'non_hopeful_ideal_source_is_emergency_battle',
    'identity_revealed': False, 'semantically_known': False, 'identity_state': 'hidden',
}


def validate(c):
    if (c.get('kind') not in KINDS or not c.get('run_revision') or not c.get('generations')
            or not c.get('start_sequence') or not c.get('artifact_sequences') or not c.get('review_ids')
            or not c.get('approval_id') or not c.get('reason')):
        raise ValueError('Reviewed notebook correction requires approval and physical/time scope')
    if c['kind'] == 'notebook_identity':
        allowed = {'type', 'node_type', 'name', 'node_name', 'stage_name', 'fate_event',
                   'identity_source', 'identity_from_prediction', 'prediction_rule',
                   'identity_revealed', 'semantically_known', 'identity_state', 'identity_unrecoverable'}
        if not c.get('when') or not c.get('fields') or set(c['fields']) - allowed:
            raise ValueError('Notebook repair may only restore reviewed identity fields')
    elif (c['floor'] != 1 or c.get('to', {}).get('type') != 'battle_elite'
          or not c.get('center_evidence')):
        raise ValueError('Ideal-source correction requires reviewed first-floor center evidence')


def correct(value, c, context=None, container='', notebook=False):
    context = context or {}
    if isinstance(value, list):
        return [correct(v, c, context, container, notebook) for v in value]
    if not isinstance(value, dict):
        return value
    ctx = {**context, **{k: value[k] for k in CONTEXT if k in value}}
    if 'artifact_set_id' in value:
        # A missing mapping is unknown, never a license to backfill earlier views.
        ctx['sequence'] = c['artifact_sequences'].get(value['artifact_set_id'])
    result = dict(value)
    for key in CONTAINERS:
        if key in value:
            result[key] = correct(value[key], c, ctx, key,
                                  notebook or key in ('exploration_notebook', 'exploration_note_nodes'))
    seq = ctx.get('sequence')
    if (ctx.get('floor') != c['floor'] or ctx.get('run_revision') != c['run_revision']
            or ctx.get('map_generation') not in c['generations'] or seq is None
            or seq < c['start_sequence'] or (c.get('end_sequence') and seq >= c['end_sequence'])):
        return result
    is_node = value.get('id') == c['node']
    if c['kind'] == 'notebook_identity':
        if not notebook or not is_node or not any(
                all(value.get(k) == expected for k, expected in match.items()) for match in c['when']):
            return result
        result.update(c['fields'])
    else:
        page = (ctx.get('transaction_id') == c['transaction_id']
                and value.get('node', c['node']) == c['node']
                and (container in ('page', 'attribution') or value.get('node') == c['node']))
        eligible_node = is_node and value.get('identity_source') in (
            'entered_page', 'archive_revision', 'ideal_source_emergency_prediction')
        if not (page or eligible_node) or value.get('node_type', value.get('type')) not in (
                'battle_normal', 'hide_battle', 'battle_elite'):
            return result
        for key in ('node_type', 'type'):
            if key in value:
                result[key] = 'battle_elite'
        result.update(PREDICTION)
        if 'identity_unrecoverable' in value:
            result['identity_unrecoverable'] = False
        for key in ('name', 'node_name'):
            if value.get(key) in ('作战', '未知的凶戾'):
                result[key] = '紧急作战'
        # A battle title can survive in the completed battle payload even when a
        # later prediction erases the notebook's title. Never use future evidence.
        if notebook and is_node and not value.get('stage_name'):
            stage = value.get('battle', {}).get('stage_name')
            if stage:
                result['stage_name'] = stage
    if result == value:
        return result
    prior = value.get('node_type_correction')
    result['node_type_correction'] = {
        'method': 'archive-reviewed-identity-1', 'review_ids': c['review_ids'],
        'approval_id': c['approval_id'], 'transaction_id': c['transaction_id'], 'reason': c['reason'],
        'raw_identity': {k: value[k] for k in ('type', 'node_type', 'name', 'node_name', 'stage_name',
            'identity_source', 'identity_revealed', 'semantically_known', 'identity_from_prediction',
            'prediction_rule', 'identity_state', 'fate_event', 'identity_unrecoverable') if k in value},
    }
    if prior:
        result['node_type_correction']['previous_correction'] = prior
    return result
