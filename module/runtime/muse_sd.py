"""Removable FAT/exFAT storage on Module LLM. Never target onboard eMMC."""

import contextlib
import errno
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import stat
import subprocess
import threading
import time


MOUNT = Path('/run/muse-storyteller/sd')
EXPORTS = 'Muse Exports'
FILESYSTEMS = ('vfat', 'exfat')
PAGE = 4
CHUNK = 256 * 1024


def display(text, size=80):
    clean = ''.join(char if char.isprintable() else ' ' for char in str(text))
    return clean.encode('utf-8', 'replace')[:size].decode('utf-8', 'ignore')


def filename(name):
    if (not isinstance(name, str) or not name or len(name.encode('utf-8')) > 96
            or name.startswith('.') or name != name.strip() or name.endswith('.')
            or any(not c.isprintable() or c in '<>:"/\\|?*' for c in name)
            or re.fullmatch(r'(?i:CON|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])', name.split('.')[0])):
        raise ValueError('Use a simple name without slashes or a leading dot')
    return name


def run(*args, timeout=10):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)


def mounts():
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        main, extra = line.split(' - ', 1)
        fields = main.split()
        yield fields[2], fields[4], extra.split()[0], fields[5].split(',')


class LinuxCard:
    def __init__(self, root=MOUNT):
        self.root = Path(root)
        self.marker = self.root.parent / 'card-id'

    def detect(self):
        disks = []
        for disk in Path('/sys/class/block').glob('mmcblk*'):
            if not re.fullmatch(r'mmcblk[1-9][0-9]*', disk.name):
                continue
            try:
                if (disk / 'device/type').read_text().strip() == 'SD':
                    disks.append(disk)
            except OSError:
                continue
        if not disks:
            return None
        if len(disks) != 1:
            raise ValueError('Multiple SD devices found; no card selected')
        disk = disks[0]
        partitions = sorted(disk.glob(disk.name + 'p*'))
        if len(partitions) > 1:
            raise ValueError('Use a card with one data partition')
        volume = partitions[0] if partitions else disk
        cid = (disk / 'device/cid').read_text().strip()
        if not re.fullmatch(r'[0-9a-fA-F]{32}', cid):
            raise ValueError('Cannot verify this card identity')
        node = Path('/dev') / volume.name
        info = node.stat()
        number = (volume / 'dev').read_text().strip()
        if not stat.S_ISBLK(info.st_mode) or number != f'{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}':
            raise ValueError('Cannot verify the card device')
        device_numbers = tuple((part / 'dev').read_text().strip() for part in [disk, *partitions])
        return (cid, str(node), number, int((volume / 'size').read_text()) * 512, bool(partitions), device_numbers)

    def mounted(self, card):
        for number, path, fs, options in mounts():
            if path == str(self.root):
                if not card or number != card[2] or fs not in FILESYSTEMS or not self.owns_mount(number, card[0]):
                    raise ValueError('Card mount changed; eject it before reuse')
                return fs, 'rw' in options
        return None

    def external(self, card):
        if not card:
            return
        platform = self.platform_mount(card)
        if any(number in card[5] and path != str(self.root) and (not platform or path != platform[0])
               for number, path, _, _ in mounts()):
            raise ValueError('Card is mounted by another service')

    def platform_mount(self, card):
        if not card:
            return None
        expected = Path('/mnt') / Path(card[1]).name
        for number, path, fs, options in mounts():
            if number == card[2] and path == str(expected) and fs in FILESYSTEMS and expected.resolve() == expected:
                return path, fs, 'rw' in options
        return None

    def probe(self, card):
        if not shutil.which('blkid'):
            raise ValueError('Card identification utility is unavailable')
        result = run('blkid', '-p', '-o', 'export', card[1])
        if result.returncode not in (0, 2):
            raise ValueError('Card filesystem could not be identified safely')
        return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def check(self, card):
        if not card or self.detect() != card:
            raise ValueError('Card changed; refresh and try again')
        self.external(card)

    def owns_mount(self, number, cid=None):
        try:
            old_cid, old_number = self.marker.read_text().split()
            return old_number == number and (cid is None or old_cid == cid)
        except (OSError, ValueError):
            return False

    def mount(self, card, readonly=False):
        self.check(card)
        if self.mounted(card):
            return
        platform = self.platform_mount(card)
        fs = platform[1] if platform else self.probe(card).get('TYPE')
        if fs not in FILESYSTEMS:
            raise ValueError('Mount requires a FAT32 or exFAT data card')
        self.root.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.root.mkdir(mode=0o700, exist_ok=True)
        if self.root.resolve() != self.root or any(self.root.iterdir()):
            raise ValueError('Card mount directory is occupied')
        self.check(card)
        marker_tmp = self.marker.with_suffix('.new')
        marker_tmp.write_text(card[0] + ' ' + card[2], encoding='ascii')
        os.replace(marker_tmp, self.marker)
        safety = 'nosuid,nodev,noexec,noatime'
        try:
            if platform:
                if run('mount', '--bind', platform[0], str(self.root)).returncode:
                    raise ValueError('Could not open the Module’s card mount')
                mode = 'ro' if readonly or not platform[2] else 'rw'
                if run('mount', '-o', f'remount,bind,{mode},{safety}', str(self.root)).returncode:
                    raise ValueError('Could not protect the Module’s card mount')
            else:
                opts = safety + ',uid=0,gid=0,umask=077' + (',ro' if readonly else '')
                if run('mount', '-t', fs, '-o', opts, card[1], str(self.root)).returncode:
                    raise ValueError('Mount failed; check the card filesystem')
            self.check(card)
            result = self.mounted(card)
            if not result or (readonly and result[1]):
                raise ValueError('Mount did not become available in the requested mode')
        except (OSError, ValueError, subprocess.SubprocessError):
            with contextlib.suppress(OSError, ValueError, subprocess.SubprocessError):
                self.unmount(card)
            raise

    def unmount(self, card, release_platform=False):
        if release_platform and card:
            self.check(card)
            platform = self.platform_mount(card)
            if platform and run('umount', platform[0], timeout=30).returncode:
                raise ValueError('The Module is still using the card; leave it inserted')
        number = next((number for number, path, _, _ in mounts() if path == str(self.root)), None)
        if not number:
            return
        if not self.owns_mount(number):
            raise ValueError('This mount belongs to another service')
        if run('umount', str(self.root), timeout=30).returncode:
            raise ValueError('Card is in use; close the PC transfer and try again')
        self.marker.unlink(missing_ok=True)

    def format_available(self):
        return bool(shutil.which('mkfs.fat') or shutil.which('mkfs.vfat'))

    def format(self, card):
        tool = shutil.which('mkfs.fat') or shutil.which('mkfs.vfat')
        if not tool:
            raise ValueError('FAT32 formatter unavailable on this Module image')
        self.check(card)
        if self.mounted(card) or self.platform_mount(card):
            raise ValueError('Eject the card before formatting')
        if not card[4] and self.probe(card).get('PTTYPE'):
            raise ValueError('Card partition table is present; use a card reader')
        self.check(card)
        if run(tool, '-F', '32', '-n', 'MUSE_SD', card[1], timeout=65).returncode:
            raise ValueError('FAT32 format did not complete')


class SdWorker:
    """Owns card operations away from the archive, audio and UART threads."""

    def __init__(self, archive, recorder, card=None):
        self.media = card or LinuxCard()
        self.archive = Path(archive) / 'conversations.sqlite3'
        self.recordings = Path(getattr(recorder, 'root', '/var/lib/muse-recordings'))
        self.lock = threading.Lock()
        self.view = dict(state='unknown', note='Refresh to check the card', path='/',
                         items=[], count=0, page=0, more=False, busy=False, done=0,
                         total=0, mounted=False, writable=False, format=False,
                         total_mb=0, free_mb=0, token='', confirm='', detail='', moving='')
        self.directory = Path('.')
        self.page = 0
        self.slots = {}
        self.confirmation = None
        self.identity = None
        self.moving = None

    def publish(self, **fields):
        with self.lock:
            self.view.update(fields)

    def submit(self, data):
        action = data.get('sd_action')
        for field in ('sd_action', 'token', 'name'):
            if field in data and not isinstance(data[field], str):
                raise ValueError('Invalid card request')
        with self.lock:
            if action == 'status' or self.view['busy']:
                return dict(self.view)
            self.view.update(busy=True, note='Working on microSD...', done=0, total=0,
                             token='', confirm='', detail='')
            result = dict(self.view)
        try:
            threading.Thread(target=self.execute, args=(action, data), name='muse-microsd', daemon=True).start()
        except RuntimeError:
            self.publish(busy=False, note='Card worker unavailable')
            return dict(self.view)
        return result

    def current(self, card, write=False):
        self.media.check(card)
        mounted = self.media.mounted(card)
        if not mounted or (write and not mounted[1]):
            raise ValueError('Mount a writable card first' if write else 'Mount the card first')
        return self.media.root / self.directory

    def selected(self, data, folder):
        item = self.slots.get(data.get('token'))
        if not item or not (folder / item).exists():
            raise ValueError('Selection changed; reopen the item')
        return folder / item

    def refresh(self, card):
        mounted = self.media.mounted(card)
        if not mounted:
            self.directory, self.page = Path('.'), 0
            self.slots = {}
            self.moving = None
            self.publish(state='unmounted' if card else 'absent', mounted=False, writable=False,
                         format=bool(card and not self.media.platform_mount(card) and self.media.format_available()), path='/', items=[],
                         page=0, more=False, count=0, free_mb=0, total_mb=(card[3] >> 20) if card else 0, moving='')
            return
        folder = self.current(card)
        if not folder.is_dir():
            self.directory, self.page, folder = Path('.'), 0, self.media.root
        count = 0
        start = self.page * PAGE
        visible = []
        with os.scandir(folder) as entries:
            for entry in entries:
                if entry.name.startswith('.'):
                    continue
                is_folder = entry.is_dir(follow_symlinks=False)
                if not is_folder and not entry.is_file(follow_symlinks=False):
                    continue
                if start <= count < start + PAGE:
                    visible.append((not is_folder, entry.name))
                count += 1
        if self.page and count <= start:
            self.page = 0
            self.refresh(card)
            return
        rows = []
        self.slots = {}
        for file, name in sorted(visible):
            token = secrets.token_hex(6)
            self.slots[token] = name
            rows.append(dict(token=token, name=display(name, 72), folder=not file))
        size = shutil.disk_usage(self.media.root)
        path = '/' if self.directory == Path('.') else '/' + self.directory.as_posix()
        self.publish(state=mounted[0], mounted=True, writable=mounted[1], format=False,
                     path=display(path, 104), total_mb=size.total >> 20, free_mb=size.free >> 20,
                     items=rows, count=count, page=self.page, more=(self.page + 1) * PAGE < count)

    def arm(self, card, operation, path=None):
        token = secrets.token_hex(8)
        self.confirmation = (operation, token, card, path, time.monotonic() + 45)
        text = 'Erase all files on this microSD? FAT32 format.' if path is None else 'Delete ' + display(path.name, 65) + (' and its contents?' if path.is_dir() else '?')
        self.publish(token=token, confirm=text)

    def confirm(self, card, action, token):
        pending, self.confirmation = self.confirmation, None
        if (not pending or pending[:3] != (action, token, card)
                or time.monotonic() > pending[4]):
            raise ValueError('Confirmation expired; select the action again')
        return pending[3]

    def execute(self, action, data):
        card = None
        try:
            card = self.media.detect()
            if card != self.identity:
                self.directory, self.page, self.slots = Path('.'), 0, {}
                self.confirmation = None
                self.moving = None
                self.publish(moving='')
                self.identity = card
            note = self.action(action, data, card)
            self.refresh(card)
            self.publish(note=note)
        except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as exc:
            with contextlib.suppress(OSError, ValueError, subprocess.SubprocessError):
                self.refresh(self.media.detect())
            if isinstance(exc, OSError) and exc.errno == errno.ENOSPC:
                message = 'Not enough free space on the card'
            elif isinstance(exc, ValueError):
                message = str(exc)
            else:
                message = 'Card operation failed; device originals are safe'
            self.publish(note=display(message, 116))
        finally:
            self.publish(busy=False)

    def action(self, action, data, card):
        if action not in ('format_do', 'delete_do'):
            self.confirmation = None
        if action == 'refresh':
            if card and self.media.platform_mount(card) and not self.media.mounted(card):
                self.media.mount(card)
            return 'Card status updated' if card else 'Insert a microSD card in Module LLM'
        if action == 'eject':
            self.media.unmount(card, release_platform=True)
            if self.media.platform_mount(card):
                self.media.mount(card)
                raise ValueError('The Module remounted the card; leave it inserted')
            return 'Ejected. Safe to remove the card'
        if not card:
            raise ValueError('No microSD card detected')
        if action == 'mount':
            self.media.mount(card)
            return 'Ready on Module LLM'
        if action == 'format_arm':
            self.media.check(card)
            if self.media.mounted(card) or self.media.platform_mount(card) or not self.media.format_available():
                raise ValueError('Eject the card first and check formatter availability')
            self.arm(card, 'format_do')
            return 'Format erases the card'
        if action == 'format_do':
            self.confirm(card, action, data.get('token'))
            self.media.format(card)
            return 'Formatted as FAT32. Tap Mount'
        folder = self.current(card, write=action in ('mkdir', 'rename', 'move_arm', 'move_here', 'delete_arm', 'delete_do', 'export'))
        if action == 'up':
            self.directory, self.page = self.directory.parent, 0
        elif action in ('previous', 'next'):
            self.page = max(0, self.page + (-1 if action == 'previous' else 1))
        elif action == 'open':
            path = self.selected(data, folder)
            if path.is_dir():
                self.directory, self.page = path.relative_to(self.media.root), 0
            else:
                detail = f'{display(path.name, 76)} | {path.stat().st_size:,} bytes'
                if path.suffix.lower() in ('.txt', '.md', '.csv', '.json', '.log'):
                    with path.open('rb') as source:
                        text = source.read(321)
                    preview = text[:320].decode('utf-8', 'replace')
                    preview = '\n'.join(display(line, 100) for line in preview.splitlines())
                    detail += '\n\n' + preview + ('\n[preview]' if len(text) > 320 else '')
                self.publish(detail=detail)
        elif action == 'mkdir':
            path = folder / filename(data.get('name'))
            if path.exists():
                raise ValueError('An item with that name already exists')
            path.mkdir()
            return 'Folder created'
        elif action == 'rename':
            path = self.selected(data, folder)
            destination = folder / filename(data.get('name'))
            if destination.exists():
                raise ValueError('An item with that name already exists')
            path.rename(destination)
            return 'Item renamed'
        elif action == 'move_arm':
            self.moving = self.selected(data, folder)
            self.publish(moving=display(self.moving.name, 72))
            return 'Open the destination folder and tap Move here'
        elif action == 'move_here':
            source = self.moving
            if source is None or not source.exists():
                raise ValueError('The source changed; select an item again')
            destination = folder / source.name
            if destination.exists() or source == folder or source in folder.parents:
                raise ValueError('Choose a different folder with no matching item')
            source.rename(destination)
            self.moving = None
            self.publish(moving='')
            return 'Item moved on the card'
        elif action == 'move_cancel':
            self.moving = None
            self.publish(moving='')
            return 'Move canceled'
        elif action == 'delete_arm':
            self.arm(card, 'delete_do', self.selected(data, folder))
            return 'Confirm removal from the card'
        elif action == 'delete_do':
            path = self.confirm(card, action, data.get('token'))
            if path.parent != folder:
                raise ValueError('Folder changed; select the item again')
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
            return 'Removed from the card'
        elif action == 'export':
            target = self.export(card, data.get('name'), data.get('token'))
            self.directory, self.page = target.relative_to(self.media.root), 0
            return 'Export complete. Device originals kept'
        else:
            raise ValueError('Unknown card action')
        return 'Browse card files'

    def export(self, card, category, recording):
        if category not in ('all', 'recordings', 'transcripts', 'conversations'):
            raise ValueError('Select an export type')
        sources = []
        if category != 'conversations':
            for folder in sorted(self.recordings.glob('rec-*')):
                if (not re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}', folder.name)
                        or folder.is_symlink() or (recording and recording != folder.name)):
                    continue
                try:
                    session = json.loads((folder / 'session.json').read_text(encoding='utf-8'))
                except (OSError, ValueError):
                    continue
                if not isinstance(session, dict) or session.get('active') is not False or session.get('state') in ('starting', 'recording', 'saving'):
                    continue
                for path in sorted(folder.iterdir()):
                    transcript = path.name in ('transcript.txt', 'reviewed.txt')
                    audio = re.fullmatch(r'audio-[0-9]{6}\.wav\.gz', path.name)
                    if (transcript or (category != 'transcripts' and audio)) and path.is_file() and not path.is_symlink():
                        sources.append((path, Path('Recordings') / folder.name / path.name))
        backup = category in ('all', 'conversations') and self.archive.is_file()
        total = len(sources) + int(backup)
        if not total:
            raise ValueError('No completed items available for this export')
        self.current(card, write=True)
        directory = self.media.root / EXPORTS
        directory.mkdir(exist_ok=True)
        suffix = secrets.token_hex(4)
        final = directory / ('Muse-' + suffix)
        stage = directory / ('.muse-' + suffix + '.partial')
        if final.exists():
            raise ValueError('Export name already exists; try again')
        stage.mkdir()
        self.publish(note='Copying to microSD...', total=total)
        try:
            for done, (src, name) in enumerate(sources, 1):
                self.current(card, write=True)
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                with src.open('rb') as input_file, path.open('xb') as output:
                    shutil.copyfileobj(input_file, output, CHUNK)
                    output.flush()
                    os.fsync(output.fileno())
                self.publish(done=done)
            if backup:
                self.current(card, write=True)
                snapshot = stage / 'conversations.sqlite3'
                with contextlib.closing(sqlite3.connect(self.archive.resolve().as_uri() + '?mode=ro', uri=True)) as source:
                    with contextlib.closing(sqlite3.connect(str(snapshot))) as dest:
                        deadline = time.monotonic() + 45
                        def progress(_status, _remaining, _pages):
                            if time.monotonic() > deadline:
                                raise ValueError('Conversation backup timed out; try again')
                        source.backup(dest, pages=64, progress=progress)
                        dest.execute('PRAGMA journal_mode=DELETE')
                        if dest.execute('PRAGMA quick_check').fetchone()[0] != 'ok':
                            raise ValueError('Conversation backup verification failed')
                self.publish(done=total)
            self.current(card, write=True)
            stage.rename(final)
        except BaseException:
            with contextlib.suppress(OSError, ValueError):
                self.current(card, write=True)
                shutil.rmtree(stage)
            raise
        return final
