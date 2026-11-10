from contextlib import closing
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'module/runtime'))
from muse_sd import LinuxCard, SdWorker, filename


class Card:
    def __init__(self, root):
        self.root = root
        root.mkdir()
        self.card = ('a' * 32, '/dev/mmcblk1p1', '179:9', 32 * 10**9, True, ('179:8', '179:9'))
        self.attached = False
        self.formats = []

    def detect(self):
        return self.card

    def mounted(self, card):
        return ('vfat', True) if self.attached and card else None

    def check(self, card):
        if not card or card != self.card:
            raise ValueError('Card changed')

    def platform_mount(self, card):
        return None

    def mount(self, card):
        self.check(card)
        self.attached = True

    def unmount(self, card, release_platform=False):
        self.attached = False

    def format_available(self):
        return True

    def format(self, card):
        self.check(card)
        self.formats.append(card)


class MicroSDTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.card = Card(self.root / 'card')
        self.recorder = self.root / 'recordings'
        self.archive = self.root / 'conversations.sqlite3'
        self.worker = SdWorker(self.root, SimpleNamespace(root=self.recorder), self.card)
        self.recording = self.recorder / 'rec-000001-abcdef01'
        self.recording.mkdir(parents=True)
        (self.recording / 'transcript.txt').write_text('Blue lantern', encoding='utf-8')
        (self.recording / 'reviewed.txt').write_text('The blue lantern', encoding='utf-8')
        (self.recording / 'audio-000001.wav.gz').write_bytes(b'example audio')
        (self.recording / 'session.json').write_text(json.dumps({'active': False, 'state': 'saved'}))

    def action(self, action, **fields):
        self.worker.execute(action, fields)
        return self.worker.view

    def item(self, name):
        return next(entry['token'] for entry in self.worker.view['items'] if entry['name'] == name)

    def test_explore_rename_and_confirm_delete(self):
        self.assertTrue(self.action('mount')['writable'])
        self.action('mkdir', name='Stories')
        self.action('rename', token=self.item('Stories'), name='Saved stories')
        self.action('open', token=self.item('Saved stories'))
        self.assertEqual(self.worker.view['path'], '/Saved stories')
        self.action('mkdir', name='October')
        self.action('up')
        self.action('delete_arm', token=self.item('Saved stories'))
        self.assertTrue((self.card.root / 'Saved stories' / 'October').is_dir())
        token = self.worker.view['token']
        self.action('delete_do', token=token)
        self.assertFalse((self.card.root / 'Saved stories').exists())

    def test_format_token_is_bound_to_ejected_card_and_single_use(self):
        self.action('mount')
        self.assertIn('Eject', self.action('format_arm')['note'])
        self.action('eject')
        self.action('format_arm')
        original_token = self.worker.view['token']
        self.card.card = ('b' * 32,) + self.card.card[1:]
        self.action('format_do', token=original_token)
        self.assertFalse(self.card.formats)
        self.action('format_arm')
        token = self.worker.view['token']
        self.action('format_do', token=token)
        self.action('format_do', token=token)
        self.assertEqual(self.card.formats, [self.card.card])

    def test_move_between_folders_and_reject_moving_into_itself(self):
        self.action('mount')
        self.action('mkdir', name='Source')
        self.action('mkdir', name='Destination')
        self.action('move_arm', token=self.item('Source'))
        self.action('open', token=self.item('Source'))
        self.assertIn('different folder', self.action('move_here')['note'])
        self.action('up')
        self.action('open', token=self.item('Destination'))
        self.action('move_here')
        self.assertTrue((self.card.root / 'Destination' / 'Source').is_dir())
        self.assertFalse((self.card.root / 'Source').exists())

    def test_onboard_emmc_is_never_a_card_candidate(self):
        emmc = Path('/sys/class/block/mmcblk0')
        with patch('muse_sd.Path.glob', return_value=[emmc]):
            self.assertIsNone(LinuxCard().detect())

    def test_export_includes_committed_wal_and_preserves_sources(self):
        with closing(sqlite3.connect(self.archive)) as db:
            db.execute('PRAGMA journal_mode=WAL')
            db.execute('CREATE TABLE messages(text)')
            db.execute('INSERT INTO messages VALUES (?)', ('private synthetic story',))
            db.commit()
            self.action('mount')
            self.action('export', name='all')
            target, = (self.card.root / 'Muse Exports').iterdir()
            with closing(sqlite3.connect(target / 'conversations.sqlite3')) as exported:
                self.assertEqual(exported.execute('SELECT text FROM messages').fetchone()[0], 'private synthetic story')
            copied = target / 'Recordings' / self.recording.name
            self.assertEqual((copied / 'audio-000001.wav.gz').read_bytes(), b'example audio')
            self.assertEqual((copied / 'transcript.txt').read_bytes(), (self.recording / 'transcript.txt').read_bytes())
            self.assertTrue((copied / 'reviewed.txt').exists())
            self.assertFalse((copied / 'session.json').exists())

    def test_partial_export_does_not_publish_or_touch_original(self):
        self.action('mount')
        with patch('muse_sd.shutil.copyfileobj', side_effect=OSError(28, 'no space')):
            self.assertIn('free space', self.action('export', name='recordings')['note'])
        self.assertEqual(list((self.card.root / 'Muse Exports').iterdir()), [])
        self.assertEqual((self.recording / 'audio-000001.wav.gz').read_bytes(), b'example audio')

    def test_selected_transcript_export_and_active_capture_excluded(self):
        self.action('mount')
        self.action('export', name='transcripts', token=self.recording.name)
        target, = (self.card.root / 'Muse Exports').iterdir()
        copied = target / 'Recordings' / self.recording.name
        self.assertEqual({path.name for path in copied.iterdir()}, {'reviewed.txt', 'transcript.txt'})
        (self.recording / 'session.json').write_text('{"active":true,"state":"recording"}')
        self.assertIn('No completed', self.action('export', name='recordings')['note'])

    def test_windows_and_linux_traversal_names_rejected(self):
        for value in ('../test', 'a/b', 'a\\b', 'NUL.txt', 'new folder.', ' hello', 'trailing '):
            with self.subTest(value=value), self.assertRaises(ValueError):
                filename(value)


if __name__ == '__main__':
    unittest.main()
