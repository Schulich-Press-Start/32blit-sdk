"""Generate SDK class definitions in a Tiled project without editing maps."""

import json
from pathlib import Path

from blit_tiled import _contract, _PROPERTY_TYPES, _replace, _require, _tile_flags, _unique_pairs


# Tiled's Project::load/save round-trips custom project properties; it drops
# arbitrary top-level keys. Store matching class name, usage and ID together.
_OWNER_PROPERTY = '32blit.sdk.generatedClasses'
_INT_MAX = 0x7FFFFFFF
_MEMBER_DEFAULTS = {'bool': False, 'int': 0, 'float': 0.0, 'string': '', 'object': 0}


def _class_definition(name, usage, members):
    return {'name': name, 'type': 'class', 'useAs': [usage], 'members': members}


def _definitions(classes, registry, flags):
    result = {}
    for name, spec in sorted(classes.items()):
        defaults = spec.get('defaults', {})
        members = {}
        for field, prop in spec.get('fields', {}).items():
            if prop.startswith('$'):
                continue
            component, field_name = field.split('.')
            kind = _PROPERTY_TYPES[registry[component][field_name]]
            if prop in members:
                _require(members[prop]['type'] == kind,
                         f'{name}: property {prop!r} maps to incompatible Tiled types')
                continue
            value = defaults.get(prop, _MEMBER_DEFAULTS[kind])
            # Tiled uses Qt signed int both for int properties and object IDs.
            _require(kind not in ('int', 'object') or value <= _INT_MAX,
                     f'{name}: default for {prop!r} is outside Tiled\'s signed integer range')
            members[prop] = {'name': prop, 'type': kind, 'value': value}
        result[name, 'object'] = _class_definition(name, 'object', [members[key] for key in sorted(members)])
    if flags:
        members = [{'name': name, 'type': 'bool', 'value': False} for name, _ in flags]
        result['TileFlags', 'tile'] = _class_definition('TileFlags', 'tile', members)
    return result


def _owned_types(properties):
    _require(isinstance(properties, list) and all(isinstance(prop, dict) for prop in properties),
             'expected a Tiled project properties array')
    markers = [prop for prop in properties if prop.get('name') == _OWNER_PROPERTY]
    if not markers:
        return None, {}
    message = f'invalid or ambiguous {_OWNER_PROPERTY!r} project property'
    _require(len(markers) == 1, message)
    marker = markers[0]
    _require(marker.get('type') == 'string' and not marker.get('propertytype')
             and isinstance(marker.get('value'), str), message)
    try:
        records = json.loads(marker['value'])
    except json.JSONDecodeError as exc:
        raise ValueError(message) from exc
    _require(isinstance(records, list), message)
    owned = {}
    ids = set()
    for record in records:
        _require(isinstance(record, list) and len(record) == 3, message)
        name, usage, type_id = record
        _require(isinstance(name, str) and name and usage in ('object', 'tile')
                 and type(type_id) is int and 0 < type_id <= _INT_MAX
                 and (name, usage) not in owned and type_id not in ids, message)
        owned[name, usage] = type_id
        ids.add(type_id)
    return marker, owned


def _update_types(entries, definitions, owned):
    _require(isinstance(entries, list), 'expected a Tiled project propertyTypes array')
    owned_by_id = {type_id: key for key, type_id in owned.items()}
    seen_ids = set()
    remaining = definitions.copy()
    merged = []
    current = {}
    for entry in entries:
        _require(isinstance(entry, dict), 'expected a Tiled property type object')
        type_id, name = entry.get('id'), entry.get('name')
        _require(isinstance(name, str) and name and type(type_id) is int and 0 < type_id <= _INT_MAX,
                 'Tiled property types must have a name and a positive signed integer ID')
        _require(type_id not in seen_ids, f'duplicate Tiled property type ID {type_id}')
        seen_ids.add(type_id)
        uses = entry.get('useAs')
        if type_id in owned_by_id:
            key = owned_by_id[type_id]
            _require(entry.get('type') == 'class' and name == key[0] and uses == [key[1]],
                     f'property type ID {type_id} no longer matches SDK-owned {key[1]} class {key[0]!r}')
            if key in remaining:
                merged.append({**entry, **remaining.pop(key)})
                current[key] = type_id
        else:
            # Tiled class lookup uses both name and usage. Enums and older classes
            # without a useAs array are only available as custom property values.
            if entry.get('type') == 'class' and isinstance(uses, list):
                for usage in ('object', 'tile'):
                    _require(usage not in uses or (name, usage) not in definitions,
                             f'Tiled {usage} class {name!r} exists and is not owned by the SDK')
            merged.append(entry)

    next_id = max(seen_ids | owned_by_id.keys(), default=0) + 1
    for key, definition in remaining.items():
        type_id = owned.get(key)
        if type_id is None:
            _require(next_id <= _INT_MAX, 'Tiled property type IDs are exhausted')
            type_id = next_id
            next_id += 1
        merged.append({'id': type_id, 'color': '#ffa0a0a4', 'drawFill': True, **definition})
        current[key] = type_id
    return merged, current


def export_project(path: Path, schema: Path, registry: Path) -> None:
    """Refresh only SDK-owned classes and their project ownership record."""
    _require(path.suffix == '.tiled-project', f'expected a .tiled-project path: {path}')
    path = path.resolve()
    _require(path not in (schema.resolve(), registry.resolve()), 'Tiled project conflicts with an input path')
    classes, components = _contract(schema, registry)
    definitions = _definitions(classes, components, _tile_flags(schema))
    try:
        with path.open(encoding='utf-8') as stream:
            document = json.load(stream, object_pairs_hook=_unique_pairs)
    except FileNotFoundError:
        document = {'folders': ['.'], 'extensionsPath': 'extensions'}
    _require(isinstance(document, dict), f'{path}: expected a Tiled project object')
    properties = document.setdefault('properties', [])
    marker, owned = _owned_types(properties)
    document['propertyTypes'], current = _update_types(document.get('propertyTypes', []), definitions, owned)
    if marker is None:
        marker = {'name': _OWNER_PROPERTY, 'type': 'string', 'propertytype': ''}
        properties.append(marker)
    marker['value'] = json.dumps([[name, usage, current[name, usage]] for name, usage in sorted(current)],
                                 separators=(',', ':'), ensure_ascii=False)
    _replace(path, (json.dumps(document, indent=4, allow_nan=False) + '\n').encode('utf-8'))
