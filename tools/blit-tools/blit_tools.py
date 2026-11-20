import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import blit_tiled


MAX_FRAME = 1024 * 1024
TIMEOUT = 5.0
POLL = 0.25
DEBOUNCE = 0.5
DEFAULT_CONNECT = ('127.0.0.1', 6132)


class ProtocolError(Exception):
    pass


def endpoint(text):
    host, _, port = text.partition(':')
    if host != '127.0.0.1' or not port.isascii() or not port.isdigit() or len(port) > 5 or not 1 <= int(port) <= 65535:
        raise argparse.ArgumentTypeError('expected 127.0.0.1:PORT (1–65535)')
    return host, int(port)


def emit_log(event):
    print(event['log'], file=sys.stderr, flush=True)


class Client:
    def __init__(self, address, timeout=TIMEOUT):
        self.socket = socket.create_connection(address, timeout=timeout)
        self.timeout = timeout
        self.buffer = bytearray()
        self.next_id = 1

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()

    def remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('debug reply timed out')
        return remaining

    def receive(self, deadline):
        scanned = 0
        while True:
            end = self.buffer.find(b'\n', scanned)
            if end >= 0:
                frame = self.buffer[:end]
                del self.buffer[:end + 1]
                try:
                    message = json.loads(frame.decode('utf-8'))
                except (ValueError, RecursionError) as exc:
                    raise ProtocolError('invalid JSON reply') from exc
                if not isinstance(message, dict):
                    raise ProtocolError('expected a JSON object')
                return message
            if len(self.buffer) > MAX_FRAME:
                raise ProtocolError('debug reply exceeds frame limit')
            scanned = len(self.buffer)
            self.socket.settimeout(self.remaining(deadline))
            chunk = self.socket.recv(min(65536, MAX_FRAME + 1 - len(self.buffer)))
            if not chunk:
                raise ProtocolError('game closed the debug connection')
            self.buffer.extend(chunk)

    def request(self, operation, payload=1):
        request_id = self.next_id
        self.next_id += 1
        data = json.dumps({'id': request_id, operation: payload}, ensure_ascii=False).encode('utf-8')
        if len(data) > MAX_FRAME:
            raise ProtocolError('debug request exceeds frame limit')
        deadline = time.monotonic() + self.timeout
        self.socket.settimeout(self.remaining(deadline))
        self.socket.sendall(data + b'\n')
        while True:
            self.remaining(deadline)
            message = self.receive(deadline)
            if 'id' not in message and 'log' in message:
                emit_log(message)
                continue
            if type(message.get('id')) is not int or message['id'] != request_id:
                raise ProtocolError('unexpected debug reply id')
            if type(message.get('ok')) is not bool:
                raise ProtocolError('debug reply has no boolean ok field')
            return message

    def hello(self):
        reply = self.request('hello')
        require_ok(reply)
        if type(reply.get('protocol')) is not int or reply['protocol'] != 1:
            raise ProtocolError('unsupported debug protocol')
        return reply


def error_text(reply):
    message = str(reply.get('error', 'debug request failed'))
    if 'file' in reply:
        location = str(reply['file'])
        if 'line' in reply:
            location += ':' + str(reply['line'])
        return f'{location}: {message}'
    return message


def require_ok(reply):
    if not reply['ok']:
        raise ProtocolError(error_text(reply))
    return reply


def run_command(client, command, machine):
    parts = command.strip().split(maxsplit=1)
    if not parts:
        return True
    verb = parts[0]
    if verb == 'get':
        if len(parts) != 2:
            print('usage: get ENTITY.COMPONENT[.FIELD]', file=sys.stderr)
            return False
        operation, payload = verb, parts[1]
    elif len(parts) == 1 and verb in {'hello', 'list', 'components', 'snapshot', 'pause', 'step', 'resume', 'stats'}:
        operation, payload = ('entities' if verb == 'list' else verb), 1
    else:
        operation, payload = 'console', command
    reply = client.request(operation, payload)
    if machine:
        print(json.dumps(reply, ensure_ascii=False), flush=True)
    elif not reply['ok']:
        print(error_text(reply), file=sys.stderr)
    else:
        fields = {key: value for key, value in reply.items() if key not in {'id', 'ok'}}
        if len(fields) == 1:
            value = next(iter(fields.values()))
            print(value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2))
        else:
            print(json.dumps(fields, ensure_ascii=False, indent=2) if fields else 'ok')
    return reply['ok']


def console(args):
    if args.commands is not None or args.script is not None or args.field is not None:
        with Client(args.connect) as client:
            client.hello()
            if args.commands is not None:
                return run_commands(client, args.commands, args.json)
            if args.field is not None:
                return watch_field(client, args.field, args.json)
            if args.script == '-':
                return run_commands(client, sys.stdin, args.json)
            with open(args.script, encoding='utf-8-sig') as source:
                return run_commands(client, source, args.json)
    while True:
        try:
            command = input('blit> ' if not args.json and sys.stdin.isatty() else '').strip()
        except EOFError:
            return 0
        if command in {'quit', 'exit'}:
            return 0
        if command:
            try:
                # Connect per interactive command, so the next one works after a restart.
                with Client(args.connect) as client:
                    run_command(client, command, args.json)
            except (OSError, ProtocolError) as exc:
                print(f'blit: {exc}', file=sys.stderr)


def watch_field(client, field, machine):
    previous = object()
    while True:
        reply = require_ok(client.request('get', field))
        value = reply['value']
        if value != previous:
            print(json.dumps(reply if machine else value, ensure_ascii=False), flush=True)
            previous = value
        time.sleep(POLL)


def run_commands(client, commands, machine):
    ok = True
    for command in commands:
        command = command.rstrip('\r\n')
        if not command.lstrip().startswith('#'):
            ok = run_command(client, command, machine) and ok
    return 0 if ok else 1


def file_stamp(path):
    try:
        state = path.stat()
        return state.st_mtime_ns, state.st_ctime_ns, state.st_size
    except FileNotFoundError:
        return None


WATCH_SUFFIXES = {
    '.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.cmake',
    '.sps', '.lua', '.json', '.yaml', '.yml', '.txt', '.tmx', '.tsx', '.png',
    '.jpg', '.jpeg', '.bmp', '.wav', '.ogg', '.mp3', '.ttf', '.otf', '.bin',
}


def source_stamp(paths, build_dir, scene_files):
    stamps = {}
    for path in paths:
        if path in scene_files:
            continue
        if not path.is_dir():
            stamps[path] = file_stamp(path)
            continue
        for root, dirs, files in os.walk(path):
            root = Path(root)
            dirs[:] = [name for name in dirs if not name.startswith('.') and
                       name not in {'CMakeFiles', '__pycache__', 'node_modules'} and
                       root / name != build_dir]
            for name in files:
                entry = root / name
                if entry not in scene_files and entry.suffix.lower() in WATCH_SUFFIXES:
                    stamps[entry] = file_stamp(entry)
    return stamps


def build(build_dir, target, config):
    return subprocess.run(['cmake', '--build', str(build_dir), '--config', config, '--target', target]).returncode == 0


class Game:
    def __init__(self, exe, project, address, snapshot=None):
        self.image = None
        self.process = None
        image = tempfile.NamedTemporaryFile(prefix=exe.stem + '.blit-dev-', suffix=exe.suffix,
                                            dir=exe.parent, delete=False)
        self.image = Path(image.name)
        image.close()
        try:
            shutil.copy2(exe, self.image)
            self.command = [str(self.image), '--bridge', f'{address[0]}:{address[1]}']
            if snapshot is not None:
                self.command.extend(['--restore', str(snapshot)])
            self.project = project
            self.address = address
        except BaseException:
            self.close()
            raise

    def start(self):
        self.process = subprocess.Popen(self.command, cwd=self.project)
        deadline = time.monotonic() + 10
        while True:
            if self.process.poll() is not None:
                raise ProtocolError(f'game exited before hello (status {self.process.returncode})')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f'game did not answer hello on {self.address[0]}:{self.address[1]}')
            try:
                client = Client(self.address, timeout=min(remaining, 0.5))
            except (ConnectionRefusedError, TimeoutError):
                time.sleep(min(POLL, remaining))
                continue
            with client:
                client.timeout = min(TIMEOUT, max(0, deadline - time.monotonic()))
                hello = client.hello()
            if self.process.poll() is None:
                print(f"Connected: {hello.get('title', self.image.name)}", flush=True)
                return

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.image is not None:
            self.image.unlink(missing_ok=True)


def save_snapshot(address, path):
    with Client(address) as client:
        hello = client.hello()
        if not hello.get('world'):
            return None
        snapshot = require_ok(client.request('snapshot')).get('snapshot')
        if not isinstance(snapshot, str):
            raise ProtocolError('debug snapshot is not a string')
    path.write_bytes(snapshot.encode('utf-8'))
    return path


def scene_path(text):
    if (len(text.encode('utf-8')) >= 96 or any(part in {'', '.', '..'} for part in text.split('/'))
            or any(ord(char) <= 32 or ord(char) == 127 or char in '\\:' for char in text)):
        raise argparse.ArgumentTypeError('expected a project-relative scene path without whitespace')
    return text


class FileWatch:
    def __init__(self, files):
        self.files = files
        self.handled = self.observed = self.stamp()
        self.changed = 0

    def stamp(self):
        return tuple(file_stamp(path) for path in self.files)

    def ready(self, now):
        current = self.stamp()
        if current != self.observed:
            self.observed, self.changed = current, now
        elif current != self.handled and now - self.changed >= DEBOUNCE:
            self.handled = current
            return True
        return False


class SceneWatch(FileWatch):
    def __init__(self, path, files, address):
        super().__init__(files)
        self.path, self.address = path, address

    def reload(self):
        try:
            with Client(self.address) as client:
                require_ok(client.request('console', f'load_scene {self.path}'))
        except (OSError, ProtocolError) as exc:
            print(f'Scene reload failed: {exc}', file=sys.stderr, flush=True)
        else:
            print(f'Reloaded scene: {self.path}', flush=True)


class TiledWatch(FileWatch):
    def __init__(self, project, source, schema, registry):
        self.project, self.source = project, source
        self.schema, self.registry = schema, registry
        super().__init__(sorted(blit_tiled.dependencies(project, source)))

    def convert(self):
        files = sorted(blit_tiled.dependencies(self.project, self.source))
        if files != self.files:
            self.files = files
            self.handled = self.observed = self.stamp()
        blit_tiled.import_map(self.project, self.source, self.schema, self.registry)


def dev(args):
    build_dir = args.build_dir.resolve()
    exe = args.exe.resolve()
    project = args.project_dir.resolve()
    paths = [path.resolve() for path in args.watch] if args.watch else [project]
    scene_files = {project / path for path in [args.scene, *args.scene_watch] if path is not None}
    scene = SceneWatch(args.scene, scene_files, args.connect) if args.scene else None
    tiled = [TiledWatch(project, path, args.tiled_schema, args.tiled_registry) for path in args.tiled]
    managed = scene_files.union(path for watch in tiled for path in watch.files)

    def watched():
        if not args.build_on_save:
            return file_stamp(exe)
        return source_stamp(paths, build_dir, managed)

    previous = watched() if args.build_on_save else None
    if not build(build_dir, args.target, args.config):
        return 1
    if not args.build_on_save:
        previous = watched()
    observed = previous
    changed = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='blit-dev-') as tmp:
        snapshot_file = Path(tmp) / 'snapshot.txt'
        game = Game(exe, project, args.connect)
        try:
            game.start()
            while game.process.poll() is None:
                time.sleep(POLL)
                now = time.monotonic()
                reload_scene = scene is not None and scene.ready(now)
                import_failed = False
                for watch in tiled:
                    if not watch.ready(now):
                        continue
                    try:
                        watch.convert()
                    except (OSError, ValueError) as exc:
                        print(f'Tiled import failed: {exc}', file=sys.stderr, flush=True)
                        import_failed = True
                    else:
                        reload_scene = True
                    finally:
                        for path in watch.files:
                            managed.add(path)
                            if args.build_on_save:
                                previous.pop(path, None)
                                observed.pop(path, None)
                if scene is not None and reload_scene and not import_failed:
                    scene.reload()
                current = watched()
                if current != observed:
                    observed, changed = current, now
                    continue
                if current == previous or now - changed < DEBOUNCE:
                    continue
                if not args.build_on_save and current is None:
                    continue
                previous = current
                if args.build_on_save and not build(build_dir, args.target, args.config):
                    print('Build failed; keeping the running game.', file=sys.stderr)
                    continue
                try:
                    snapshot = save_snapshot(args.connect, snapshot_file)
                    replacement = Game(exe, project, args.connect, snapshot)
                except (OSError, ProtocolError) as exc:
                    print(f'Restart skipped; keeping the running game: {exc}', file=sys.stderr)
                    continue
                try:
                    game.close()
                    replacement.start()
                except BaseException:
                    replacement.close()
                    raise
                game = replacement
            return int(game.process.returncode != 0)
        finally:
            game.close()


def import_tiled(args):
    blit_tiled.import_map(args.project_dir.resolve(), args.map, args.schema, args.registry, args.depfile, args.dep_target)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog='blit', description='Native SDL debug tools')
    commands = parser.add_subparsers(dest='command', required=True)
    importer = commands.add_parser('import', help='convert a Tiled map to SDK assets')
    importer.add_argument('map', help='project-relative .tmx path')
    importer.add_argument('--project-dir', type=Path, default=Path.cwd())
    importer.add_argument('--schema', type=Path, required=True)
    importer.add_argument('--registry', type=Path, required=True)
    importer.add_argument('--depfile', type=Path, help=argparse.SUPPRESS)
    importer.add_argument('--dep-target', type=Path, help=argparse.SUPPRESS)
    flags = commands.add_parser('tile-flags', help='generate a C++ tile-flag header')
    flags.add_argument('schema', type=Path)
    flags.add_argument('output', type=Path)
    tiled_project = commands.add_parser('tiled-project', help='create or refresh SDK classes in a Tiled project')
    tiled_project.add_argument('file', type=Path, help='path to the .tiled-project workspace')
    tiled_project.add_argument('--schema', type=Path, required=True)
    tiled_project.add_argument('--registry', type=Path, required=True)
    terminal = commands.add_parser('console', help='send game commands over localhost',
                                   epilog='Commands: hello, get ENTITY.COMPONENT[.FIELD], list, components, '
                                          'snapshot, pause, step, resume, stats, exit. Other input goes '
                                          'to the game console.')
    terminal.add_argument('--connect', type=endpoint, default=DEFAULT_CONNECT, metavar='127.0.0.1:PORT',
                          help='debug endpoint (default: 127.0.0.1:6132)')
    batch = terminal.add_mutually_exclusive_group()
    batch.add_argument('--exec', dest='commands', action='append', metavar='COMMAND', help='run a command (repeatable)')
    batch.add_argument('--script', metavar='FILE', help='run commands from a UTF-8 file or - for stdin')
    batch.add_argument('--watch', dest='field', metavar='ENTITY.COMPONENT[.FIELD]', help='print changes to a value')
    terminal.add_argument('--json', action='store_true', help='emit one JSON reply per command on stdout')
    supervisor = commands.add_parser('dev', help='build, run and restart a native game with its snapshot')
    supervisor.add_argument('--build-dir', required=True, type=Path, help='configured CMake build directory')
    supervisor.add_argument('--target', required=True, help='CMake build target')
    supervisor.add_argument('--config', default='Debug', help='CMake build configuration (default: Debug)')
    supervisor.add_argument('--exe', required=True, type=Path, help='path to the native executable')
    supervisor.add_argument('--connect', type=endpoint, default=DEFAULT_CONNECT, metavar='127.0.0.1:PORT',
                            help='debug endpoint (default: 127.0.0.1:6132)')
    supervisor.add_argument('--project-dir', type=Path, default=Path.cwd(), help='game working directory and default watch root')
    supervisor.add_argument('--build-on-save', action='store_true', help='watch project files instead of executable changes')
    supervisor.add_argument('--watch', type=Path, action='append', metavar='PATH',
                            help='with --build-on-save, replace default watch root (repeatable)')
    supervisor.add_argument('--scene', type=scene_path, metavar='PATH', help='reload this project-relative scene on save')
    supervisor.add_argument('--scene-watch', type=scene_path, action='append', default=[], metavar='PATH',
                            help='also watch this included scene (repeatable; requires --scene)')
    supervisor.add_argument('--tiled', action='append', default=[], metavar='MAP', help='import this .tmx on save (repeatable)')
    supervisor.add_argument('--tiled-schema', type=Path)
    supervisor.add_argument('--tiled-registry', type=Path)
    args = parser.parse_args(argv)
    if args.command == 'dev' and args.watch and not args.build_on_save:
        supervisor.error('--watch requires --build-on-save')
    if args.command == 'dev' and args.scene_watch and not args.scene:
        supervisor.error('--scene-watch requires --scene')
    if args.command == 'dev' and args.tiled and not (args.scene and args.tiled_schema and args.tiled_registry):
        supervisor.error('--tiled requires --scene, --tiled-schema and --tiled-registry')
    try:
        if args.command == 'import':
            return import_tiled(args)
        if args.command == 'tile-flags':
            blit_tiled.export_tile_flags(args.schema, args.output)
            return 0
        if args.command == 'tiled-project':
            from blit_tiled_project import export_project
            export_project(args.file, args.schema, args.registry)
            return 0
        return console(args) if args.command == 'console' else dev(args)
    except (OSError, ProtocolError, ValueError) as exc:
        print(f'blit: {exc}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
