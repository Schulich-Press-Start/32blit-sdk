"""Convert finite Tiled maps into packed layers and a generated scene."""

import base64
import binascii
import hashlib
import json
import math
import os
import re
import struct
import tempfile
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path
from typing import Optional, Set


_TYPES = {'bool', 'int32', 'uint32', 'float', 'string', 'entity'}
_PROPERTY_TYPES = {'bool': 'bool', 'int32': 'int', 'uint32': 'int',
                   'float': 'float', 'string': 'string', 'entity': 'object'}
_GEOMETRY = {'$x', '$y', '$width', '$height', '$rotation'}
_IDENT = re.compile(r'[A-Za-z_][A-Za-z_0-9]*\Z')
_DECIMAL = re.compile(r'-?[0-9]+\Z')
_REAL = re.compile(r'-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z')


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _relative(source, allow_parent=False):
    _require(isinstance(source, str) and source and not any(
        ord(c) < 32 or ord(c) == 127 or c in '\\:<>|?*"' for c in source),
        f'invalid project-relative path: {source!r}')
    _require(all(part and (part == '..' and allow_parent or not part.endswith(('.', ' ')))
                 for part in source.split('/')), f'invalid project-relative path: {source!r}')
    return Path(source)


def _inside(project, candidate):
    resolved = candidate.resolve()
    try:
        resolved.relative_to(project)
    except ValueError:
        raise ValueError(f'path escapes project: {candidate}') from None
    return resolved


def scene_path(source: str) -> str:
    path = _relative(source)
    _require(path.suffix == '.tmx', f'expected a .tmx source: {source!r}')
    scene = 'generated/' + path.with_suffix('.generated.scene').as_posix()
    _require(len(scene.encode('utf-8')) < 96, f'generated scene path is too long: {scene}')
    return scene


def _xml(path, tag):
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, LookupError) as exc:
        raise ValueError(f'{path}: {exc}') from exc
    _require(root.tag == tag, f'{path}: expected <{tag}>')
    return root


def _tileset_path(project, tmx, element):
    source = element.get('source')
    if source is None:
        return None
    path = _relative(source, allow_parent=True)
    _require(path.suffix == '.tsx', f'expected a .tsx tileset: {source!r}')
    return _inside(project, tmx.parent / path)


def dependencies(project: Path, source: str) -> Set[Path]:
    scene_path(source)
    project = project.resolve()
    tmx = _inside(project, project / source)
    paths = {tmx}
    for element in _xml(tmx, 'map').findall('tileset'):
        tsx = _tileset_path(project, tmx, element)
        if tsx is not None:
            paths.add(tsx)
    return paths


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, f'duplicate JSON key: {key!r}')
        result[key] = value
    return result


def _read_contract(path, key, *, optional=False):
    with path.open(encoding='utf-8') as stream:
        document = json.load(stream, object_pairs_hook=_unique_pairs)
    _require(isinstance(document, dict) and type(document.get('version')) is int
             and document['version'] == 1,
             f'{path}: expected schema version 1')
    values = document.get(key, {} if optional else None)
    _require(isinstance(values, dict), f'{path}: expected a {key!r} object')
    return values


def _tile_flags(schema):
    flags = _read_contract(schema, 'tile_flags', optional=True)
    bits = set()
    for name, bit in flags.items():
        _require(re.fullmatch(r'[a-z][a-z0-9_]*', name), f'{schema}: invalid tile flag {name!r}')
        _require(type(bit) is int and 0 <= bit < 32 and bit not in bits,
                 f'{schema}: tile flag {name!r} needs a unique bit in 0..31')
        bits.add(bit)
    return sorted(flags.items(), key=lambda entry: entry[1])


def _flag_schema(flags):
    canonical = json.dumps(flags, separators=(',', ':')).encode('ascii')
    return int.from_bytes(hashlib.sha256(canonical).digest()[:8], 'little')


def export_tile_flags(schema, output):
    flags = _tile_flags(schema)
    lines = ['#pragma once', '#include <cstdint>', '', 'namespace sdk {',
             '  enum class TileFlag : std::uint32_t {']
    lines.extend(f'    is_{name} = 0x{1 << bit:08x}u,' for name, bit in flags)
    lines.extend(['  };', f'  inline constexpr std::uint64_t tile_flag_schema = 0x{_flag_schema(flags):016x}ULL;', '}', ''])
    _replace(output, '\n'.join(lines).encode('ascii'))


def _bare_key(key):
    return isinstance(key, str) and 0 < len(key) < 64 and all(
        32 < ord(c) < 127 and c not in '"\\=#:' for c in key)


def _contract(schema, registry):
    components = _read_contract(registry, 'components')
    for component, fields in components.items():
        _require(_IDENT.fullmatch(component) and isinstance(fields, dict),
                 f'invalid registry component: {component!r}')
        for field, kind in fields.items():
            _require(_IDENT.fullmatch(field) and isinstance(kind, str) and kind in _TYPES,
                     f'invalid registry field: {component}.{field}')
    classes = _read_contract(schema, 'classes')
    for name, spec in classes.items():
        _require(name and isinstance(spec, dict) and
                 not (spec.keys() - {'shape', 'prefab', 'fields', 'components', 'defaults'}),
                 f'invalid schema class: {name!r}')
        shape = spec.get('shape')
        _require(shape in ('point', 'rectangle'), f'{name}: expected point or rectangle shape')
        if 'prefab' in spec:
            _require(_bare_key(spec['prefab']), f'{name}: invalid prefab key')
        added = spec.get('components', [])
        _require(isinstance(added, list), f'{name}: components must be a list')
        seen = set()
        for component in added:
            _require(isinstance(component, str) and component != 'Name' and component in components,
                     f'{name}: unknown or reserved component {component!r}')
            _require(component not in seen, f'{name}: duplicate component {component!r}')
            seen.add(component)
        mapped = spec.get('fields', {})
        _require(isinstance(mapped, dict), f'{name}: fields must be an object')
        defaults = spec.get('defaults', {})
        _require(isinstance(defaults, dict), f'{name}: defaults must be an object')
        unused_defaults = set(defaults)
        for key, value in mapped.items():
            component, _, field = key.partition('.')
            _require(component != 'Name' and field in components.get(component, {}),
                     f'{name}: unknown or reserved field {key!r}')
            _require(isinstance(value, str) and value, f'{name}: invalid mapping for {key}')
            if value.startswith('$'):
                _require(value in _GEOMETRY and (shape == 'rectangle' or value not in ('$width', '$height'))
                         and components[component][field] in ('float', 'int32', 'uint32'),
                         f'{name}: incompatible geometry mapping {key}={value}')
            elif value in defaults:
                unused_defaults.discard(value)
                kind = components[component][field]
                label = f'{name}: default for {value!r} ({key})'
                text = _default_text(kind, defaults[value], label)
                if kind == 'entity':
                    # Check existence only once the map's object IDs are known.
                    _integer(text, 0, 0xFFFFFFFF, label)
                else:
                    _scalar(kind, text, label, {})
        _require(not unused_defaults, f'{name}: defaults for unmapped properties {sorted(unused_defaults)!r}')
    return classes, components


def _integer(text, low, high, label):
    _require(text is not None and _DECIMAL.fullmatch(text), f'{label}: expected an integer')
    value = int(text)
    _require(low <= value <= high, f'{label}: integer outside [{low}, {high}]')
    return value


def _real(text, label):
    _require(text is not None and _REAL.fullmatch(text), f'{label}: expected a number')
    value = float(text)
    _require(math.isfinite(value), f'{label}: non-finite number')
    return value


def _children(element, allowed, label):
    for child in element:
        _require(child.tag in allowed, f'{label}: unsupported <{child.tag}>')


def _neutral_layer(element, label):
    for key, default in (('x', 0), ('y', 0), ('offsetx', 0), ('offsety', 0),
                         ('parallaxx', 1), ('parallaxy', 1), ('opacity', 1), ('visible', 1)):
        _require(_real(element.get(key, str(default)), label) == default,
                 f'{label}: unsupported {key}={element.get(key)!r}')
    _require('tintcolor' not in element.attrib and 'mode' not in element.attrib,
             f'{label}: unsupported tint or blend mode')


def _atlas(project, tmx, root, flags):
    tilesets = root.findall('tileset')
    _require(len(tilesets) == 1, 'expected exactly one atlas tileset')
    reference = tilesets[0]
    first = _integer(reference.get('firstgid'), 1, 0x0FFFFFFF, 'tileset firstgid')
    tsx = _tileset_path(project, tmx, reference)
    if tsx is not None:
        _children(reference, (), 'external tileset reference')
    tileset = _xml(tsx, 'tileset') if tsx is not None else reference
    _children(tileset, ('image', 'tile', 'properties', 'grid'), 'tileset')
    images = tileset.findall('image')
    _require(len(images) == 1 and images[0].get('source') and not len(images[0]),
             'tileset must have one external atlas image')
    for key in ('tilewidth', 'tileheight'):
        _integer(tileset.get(key), 8, 8, f'tileset {key}')
    for key in ('spacing', 'margin'):
        _integer(tileset.get(key, '0'), 0, 0, f'tileset {key}')
    count = _integer(tileset.get('tilecount'), 1, 0xFFFF, 'tileset tilecount')
    columns = _integer(tileset.get('columns'), 1, count, 'tileset columns')
    _require(count % columns == 0, 'tileset atlas must be rectangular')
    for key, expected in (('width', columns * 8), ('height', count // columns * 8)):
        if key in images[0].attrib:
            _integer(images[0].get(key), expected, expected, f'atlas image {key}')
    image = _inside(project, (tsx or tmx).parent / _relative(images[0].get('source'), allow_parent=True))
    atlas = [image.relative_to(project).as_posix(), images[0].get('trans', '')]
    grids = tileset.findall('grid')
    _require(len(grids) <= 1 and all(grid.get('orientation') == 'orthogonal' for grid in grids),
             'tileset grid must be orthogonal')
    values = [0] * 256  # runtime IDs are one-based; 0 is empty
    labels = dict(flags)
    seen = set()
    for tile in tileset.findall('tile'):
        tile_id = _integer(tile.get('id'), 0, count - 1, 'tileset tile id')
        _require(tile_id not in seen, f'duplicate tileset tile id {tile_id}')
        seen.add(tile_id)
        label = f'{tsx or tmx}: tileset tile {tile_id}'
        _children(tile, ('properties',), label)
        properties = tile.findall('properties')
        _require(len(properties) <= 1, f'{label}: duplicate properties element')
        if not properties:
            continue
        seen_flags = set()
        for prop in properties[0]:
            name = prop.get('name')
            if name not in labels:
                continue
            _require(name not in seen_flags, f'{label}: duplicate flag {name!r}')
            seen_flags.add(name)
            raw = prop.get('value', prop.text or '')
            _require(prop.tag == 'property' and not len(prop) and prop.get('type') == 'bool' and
                     raw in ('true', 'false', '1', '0'), f'{label}: {name!r} must be a bool')
            _require(tile_id < 255, f'{label}: {name!r} exceeds the 8-bit runtime limit')
            if raw in ('true', '1'):
                values[tile_id + 1] |= 1 << labels[name]
    _require(first + count - 1 <= 0x0FFFFFFF, 'tileset GIDs overlap transform flags')
    return first, count, tsx, struct.pack('<4sQ256I', b'BTP1', _flag_schema(flags), *values), atlas


def _gids(data, count, label):
    _children(data, (), label)
    text = data.text or ''
    encoding, compression = data.get('encoding'), data.get('compression')
    if encoding == 'csv' and not compression:
        values = text.split(',')
        _require(len(values) == count, f'{label}: expected {count} CSV tile IDs')
        return (_integer(value.strip(), 0, 0xFFFFFFFF, label) for value in values)
    _require(encoding == 'base64' and compression in (None, '', 'zlib'),
             f'{label}: expected CSV or base64 (optionally zlib-compressed)')
    try:
        raw = base64.b64decode(''.join(text.split()), validate=True)
        if compression == 'zlib':
            decoder = zlib.decompressobj()
            raw = decoder.decompress(raw, count * 4 + 1)
            _require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                     f'{label}: invalid zlib stream or tile count')
    except (binascii.Error, ValueError, zlib.error) as exc:
        raise ValueError(f'{label}: invalid base64/zlib tile data: {exc}') from exc
    _require(len(raw) == count * 4, f'{label}: expected {count * 4} tile data bytes')
    return (value for value, in struct.iter_unpack('<I', raw))


def _layer_name(layer, seen):
    name = layer.get('name', '')
    encoded = name.encode('utf-8')
    _require(0 < len(encoded) <= 255 and '\x00' not in name, 'invalid or empty layer name')
    _require(name not in seen, f'duplicate layer name: {name!r}')
    seen.add(name)
    return name, encoded


def _pack_layer(layer, name, encoded, width, height, first, tile_count, order):
    _children(layer, ('data', 'properties'), name)
    _integer(layer.get('width'), width, width, f'{name} width')
    _integer(layer.get('height'), height, height, f'{name} height')
    data = layer.findall('data')
    _require(len(data) == 1, f'{name}: expected one data element')
    count = width * height
    gids = _gids(data[0], count, name)
    tiles, transforms = bytearray(count), bytearray(count)
    for index, gid in enumerate(gids):
        _require(not (gid & 0x10000000), f'{name}: unsupported hexagonal rotation flag')
        tile, flags = gid & 0x0FFFFFFF, gid >> 29
        if tile:
            _require(first <= tile < first + tile_count, f'{name}: tile GID {tile} outside atlas')
            local = tile - first + 1
            _require(local <= 255, f'{name}: tile GID {tile} exceeds the 8-bit runtime limit')
            tiles[index] = local
            # Tiled swaps axes before flipping; Map swaps after flipping.
            transforms[index] = (1 | ((flags & 2) << 1) | ((flags & 4) >> 1)) if flags & 1 else flags
        else:
            _require(not flags, f'{name}: transform on an empty tile')
    return bytes((len(encoded),)) + encoded + struct.pack('<i', order) + tiles + transforms


def _quoted(text):
    parts = ['"']
    for byte in text.encode('utf-8'):
        if byte in (34, 92):
            parts.append('\\' + chr(byte))
        elif byte < 32 or byte >= 127:
            parts.append(f'\\{byte:03o}')
        else:
            parts.append(chr(byte))
    return ''.join(parts) + '"'


def _short_string(value, label):
    _require('\x00' not in value and len(value.encode('utf-8')) < 24,
             f'{label}: expected at most 23 UTF-8 bytes and no NUL')
    return _quoted(value)


def _default_text(kind, value, label):
    types = {'bool': (bool,), 'string': (str,), 'float': (int, float)}.get(kind, (int,))
    _require(type(value) in types, f'{label}: expected a {kind} default')
    if kind == 'bool':
        return 'true' if value else 'false'
    return str(value)


def _scalar(kind, text, label, keys, geometry=False):
    if kind == 'string':
        return _short_string(text, label)
    if kind == 'bool':
        _require(text in ('true', 'false', '1', '0'), f'{label}: invalid bool')
        return 'true' if text in ('true', '1') else 'false'
    if kind == 'entity':
        target = _integer(text, 0, 0xFFFFFFFF, label)
        _require(not target or target in keys, f'{label}: unknown object reference {target}')
        return '@' + keys[target] if target else 'null'
    if kind == 'float':
        value = _real(text, label)
        try:
            value32, = struct.unpack('<f', struct.pack('<f', value))
        except OverflowError:
            raise ValueError(f'{label}: float32 overflow') from None
        _require(math.isfinite(value32) and (value == 0 or value32 != 0),
                 f'{label}: number outside float32 range')
        return format(value32, '.9g')
    low, high = (0, 0xFFFFFFFF) if kind == 'uint32' else (-0x80000000, 0x7FFFFFFF)
    if geometry:
        value = _real(text, label)
        _require(value.is_integer() and low <= value <= high, f'{label}: geometry does not fit {kind}')
        return str(int(value))
    return str(_integer(text, low, high, label))


def _properties(obj, label):
    groups = obj.findall('properties')
    _require(len(groups) <= 1, f'{label}: duplicate properties element')
    if not groups:
        return {}
    _children(groups[0], ('property',), label)
    properties = {}
    for prop in groups[0]:
        name = prop.get('name')
        _require(name and name not in properties, f'{label}: empty or duplicate property {name!r}')
        _require(not len(prop) and 'propertytype' not in prop.attrib,
                 f'{label}: unsupported custom property {name!r}')
        kind = prop.get('type', 'string')
        _require(kind in _PROPERTY_TYPES.values(), f'{label}: unsupported property type {kind!r}')
        _require('value' not in prop.attrib or not (prop.text or '').strip(),
                 f'{label}: property {name!r} has both an attribute and text value')
        properties[name] = (kind, prop.get('value', prop.text or ''))
    return properties


def _entity(obj, key, label, keys, classes, registry):
    class_name = obj.get('class') or obj.get('type')
    _require(not (obj.get('class') and obj.get('type') and obj.get('class') != obj.get('type')),
             f'{label}: conflicting class and type')
    _require(class_name in classes, f'{label}: unknown Tiled class {class_name!r}')
    spec = classes[class_name]
    _require('gid' not in obj.attrib and 'template' not in obj.attrib,
             f'{label}: tile objects and templates are unsupported')
    _children(obj, ('properties', 'point'), label)
    points = obj.findall('point')
    _require(len(points) == (1 if spec['shape'] == 'point' else 0),
             f'{label}: expected {spec["shape"]} geometry')
    geometry = {'$' + name: obj.get(name, '0') for name in ('x', 'y', 'width', 'height', 'rotation')}
    numbers = {name: _real(value, f'{label} {name}') for name, value in geometry.items()}
    if points:
        _require(not len(points[0]) and numbers['$width'] == 0 and numbers['$height'] == 0,
                 f'{label}: invalid point geometry')
    else:
        _require(numbers['$width'] > 0 and numbers['$height'] > 0, f'{label}: invalid rectangle dimensions')
    properties = _properties(obj, label)
    defaults = spec.get('defaults', {})
    components = {component: [] for component in spec.get('components', [])}
    for target, mapping in spec.get('fields', {}).items():
        component, field = target.split('.')
        kind = registry[component][field]
        field_label = f'{label} {target}'
        if mapping in geometry:
            value = _scalar(kind, geometry[mapping], field_label, keys, geometry=True)
        elif mapping in properties:
            prop_type, raw = properties[mapping]
            _require(prop_type == _PROPERTY_TYPES[kind],
                     f'{field_label}: property {mapping!r} must have Tiled type {_PROPERTY_TYPES[kind]}')
            value = _scalar(kind, raw, field_label, keys)
        else:
            _require(mapping in defaults, f'{field_label}: missing property {mapping!r}')
            value = _scalar(kind, _default_text(kind, defaults[mapping], field_label), field_label, keys)
        components.setdefault(component, []).append(f'{field}={value}')
    prefab = f' : {spec["prefab"]}' if 'prefab' in spec else ''
    name = _short_string(obj.get('name', ''), f'{label} name')
    return [f'entity {key}{prefab} name={name}'] + [
        '  ' + ' '.join([component, *fields]) for component, fields in components.items()]


def _compile(project, source, tmx, classes, registry, flags):
    root = _xml(tmx, 'map')
    _require(root.get('orientation') == 'orthogonal' and root.get('infinite', '0') == '0',
             'expected a finite orthogonal map')
    _require(root.get('renderorder', 'right-down') == 'right-down', 'unsupported map render order')
    _children(root, ('tileset', 'layer', 'objectgroup', 'properties'), 'map')
    for key in ('tilewidth', 'tileheight'):
        _integer(root.get(key), 8, 8, f'map {key}')
    width = _integer(root.get('width'), 1, 0xFFFF, 'map width')
    height = _integer(root.get('height'), 1, 0xFFFF, 'map height')
    _require(width * height <= 0x7FFFFFFF, 'map exceeds signed engine tile index range')
    first, tile_count, tsx, tile_props, atlas = _atlas(project, tmx, root, flags)
    layer_count = len(root.findall('layer'))
    _require(layer_count, 'expected at least one tile layer')
    _require(layer_count <= 0xFFFF, 'too many tile layers')
    layers, layer_names, objects, object_names, keys, stable_keys = [], set(), [], set(), {}, set()
    for layer in root:
        if layer.tag not in ('layer', 'objectgroup'):
            continue
        name, encoded = _layer_name(layer, layer_names)
        _neutral_layer(layer, name)
        if layer.tag == 'layer':
            # Tiled lists layers back-to-front; smaller values draw closer to the front.
            layers.append(_pack_layer(layer, name, encoded, width, height, first, tile_count,
                                      layer_count - len(layers)))
            continue
        _children(layer, ('object', 'properties'), name)
        for obj in layer.findall('object'):
            object_id = _integer(obj.get('id'), 1, 0xFFFFFFFF, f'{name} object ID')
            label = f'{name} object {object_id}'
            _require(object_id not in keys, f'{label}: duplicate object ID')
            key = 'tiled_' + hashlib.sha256(f'{source}\0{object_id}'.encode('utf-8')).hexdigest()[:32]
            _require(key not in stable_keys, f'{label}: duplicate stable key')
            keys[object_id] = key
            stable_keys.add(key)
            object_name = obj.get('name', '')
            if object_name:
                _require(object_name not in object_names, f'{label}: duplicate entity name {object_name!r}')
                object_names.add(object_name)
            objects.append((obj, key, label))
    lines = []
    for obj, key, label in objects:
        lines.extend(_entity(obj, key, label, keys, classes, registry))
    packed = struct.pack('<4sHHHH', b'BTMP', 2, width, height, layer_count) + b''.join(layers)
    return packed, lines, tsx, tile_props, atlas


def _replace(path, content):
    try:
        if path.read_bytes() == content:
            return
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def _dep_word(path):
    # CMake consumes GCC-style make depfiles. Keep slash separators including on Windows.
    return (path.as_posix().replace('$', '$$').replace('#', r'\#')
            .replace(' ', r'\ ').replace('\t', '\\\t'))


def import_map(project: Path, source: str, schema: Path, registry: Path,
               depfile: Optional[Path] = None, dep_target: Optional[Path] = None) -> None:
    """Publish the generated scene only after its content-addressed map is ready."""
    scene = scene_path(source)
    project = project.resolve()
    tmx = _inside(project, project / source)
    scene_file = _inside(project, project / scene)
    schema, registry = schema.resolve(), registry.resolve()
    try:
        classes, components = _contract(schema, registry)
        packed, lines, tsx, tile_props, atlas = _compile(project, source, tmx, classes, components, _tile_flags(schema))
    except ValueError as exc:
        raise ValueError(f'{source}: {exc}') from exc
    generation = hashlib.sha256(packed)
    generation.update(tile_props)
    digest = generation.hexdigest()[:32]
    map_name = 'generated/' + source[:-4] + '-' + digest + '.map'
    map_file = _inside(project, project / map_name)
    props_file = _inside(project, project / (map_name + '.props'))
    scene_text = '\n'.join(['scene 1 Tiled', '# blit-map ' + map_name,
                            '# blit-atlas ' + json.dumps([*atlas, source], ensure_ascii=False), *lines, '']).encode('utf-8')
    inputs = {tmx, schema, registry}
    if tsx is not None:
        inputs.add(tsx)
    outputs = {map_file, props_file, scene_file}
    _require(len(outputs) == 3 and not (outputs & inputs),
             'generated output conflicts with another import path')
    if depfile is not None:
        depfile = depfile.resolve()
        _require(depfile not in inputs | outputs, 'depfile conflicts with an import path')
        target = dep_target.resolve() if dep_target is not None else scene_file
        dep_text = _dep_word(target) + ': ' + ' '.join(map(_dep_word, sorted(inputs))) + '\n'
    for path, content in ((map_file, packed), (props_file, tile_props)):
        try:
            existing = path.read_bytes()
        except FileNotFoundError:
            _replace(path, content)
        else:
            _require(existing == content, f'content-addressed asset has unexpected contents: {path}')
    if depfile is not None:
        _replace(depfile, dep_text.encode('utf-8'))
    _replace(scene_file, scene_text)
