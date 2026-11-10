import contextlib
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'module/runtime'))
from muse_usage import ACCURACY_LIMIT, DAY, HISTORY_LIMIT, Usage, learn


class AdaptiveUsage(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.usage = Usage(temporary.name)
        self.stamp = 1794247981

    def observe(self, used, reset='Nov 13', advance=DAY, **extra):
        self.stamp += advance
        return self.usage.calibrate(dict(used=used, reset=reset, checked='Nov 9 10:13', observed_at=self.stamp, **extra))

    def work(self, sent=0, reply=0, turns=0):
        with self.usage.state() as state:
            state['counts']['sent'] += sent
            state['counts']['reply'] += reply
            state['counts']['turns'] += turns
            state['chars'] += sent + reply

    def test_sync_counts_both_directions_once_and_skips_test_messages(self):
        with contextlib.closing(sqlite3.connect(':memory:')) as db:
            db.executescript("create table messages (seq integer, role, text, chat); create table settings (key, value);")
            self.usage.sync(db)
            db.executemany('insert into messages values (?,?,?,?)', [(1,'user','hello','c'),(2,'assistant','hi','c'),(3,'user','fake','d')])
            db.execute("insert into settings values ('v12_fixture:d','owned')")
            self.usage.sync(db)
            self.usage.sync(db)
            db.execute('delete from messages where seq=3')
            self.usage.sync(db)
        with self.usage.state(write=False) as state:
            self.assertEqual(state['counts'], dict(sent=5, reply=2, turns=1))
            self.assertEqual(state['last_seq'], 3)

    def test_migration_and_weekly_reset_preserve_existing_learning(self):
        self.usage.path.write_text(json.dumps(dict(v=1, chars=4000, last_seq=90, rate=0.001, anchor=dict(
            used=11, reset='Nov 13', checked='Nov 9 10:13', chars=4000, received=10, observed_at=self.stamp, boot='prior'))))
        self.assertEqual(self.usage.snapshot()['used'],11)
        self.work(sent=1200, reply=500, turns=1)
        self.assertEqual(self.usage.snapshot()['used'],13)
        self.observe(14)
        with self.usage.state(write=False) as state:
            self.assertEqual(len(state['history']),1)
            self.assertIsNotNone(state['local'])
        fresh=self.observe(0, 'Nov 20')
        self.assertEqual((fresh['observed'],fresh['used']), (0,0))
        self.work(reply=2000)
        self.assertGreater(self.usage.snapshot()['used'],0)

    def test_directions_learn_differently_and_ignore_saturated_samples(self):
        used=2
        self.observe(used)
        for _ in range(3):
            self.work(sent=4000,turns=2)
            used+=8
            self.observe(used, advance=3600)
            self.work(reply=5000)
            used+=2
            self.observe(used, advance=3600)
        with self.usage.state(write=False) as state:
            sent, reply=state['local']
            self.assertGreater(sent,2*reply)
            self.assertEqual(len(state['history']),6)
        self.work(sent=3000)
        self.observe(100,advance=3600)
        self.observe(45,advance=3600)  # A correction takes effect immediately.
        self.assertEqual(self.usage.snapshot()['observed'],45)
        with self.usage.state(write=False) as state:
            self.assertEqual(len(state['history']),6)

    def test_plateaus_accumulate_and_history_is_bounded(self):
        self.observe(0)
        for _ in range(3):
            self.work(reply=400)
            self.observe(0,advance=3600)
        self.observe(1,advance=3600)
        with self.usage.state(write=False) as state:
            self.assertEqual(state['history'][0]['reply'],1200)
        for i in range(HISTORY_LIMIT+3):
            self.work(reply=1000)
            self.observe(i+2,advance=3600)
        with self.usage.state(write=False) as state:
            self.assertEqual(len(state['history']),HISTORY_LIMIT)

    def test_background_requires_repeat_evidence_and_forecast_stops_at_one_day(self):
        self.observe(2)
        self.observe(5)
        with self.usage.state(write=False) as state:
            self.assertEqual(state['shared'],0)
        self.observe(8)
        self.assertEqual(self.usage.snapshot()['used'],8)
        with self.usage.state(write=False) as state:
            self.assertGreater(state['shared'],0)
            self.assertIsNone(state['local'])
            tick=state['anchor']['tick']
        with patch('muse_usage.time.monotonic',return_value=tick+DAY):
            day=self.usage.snapshot()['used']
        with patch('muse_usage.time.monotonic',return_value=tick+3*DAY):
            self.assertEqual(self.usage.snapshot()['used'],day)
        with patch.object(self.usage,'boot',return_value='new boot'), patch('muse_usage.time.time',return_value=1692653709):
            self.assertEqual(self.usage.snapshot()['used'],8)

    def test_one_outlier_is_bounded(self):
        ordinary=dict(sent=1000,reply=4000,turns=1,seconds=3600,points=1)
        spike=dict(ordinary, points=80)
        model, shared=learn([ordinary]*5+[spike],0.0002)
        self.assertLess(max(model),0.5)
        self.assertEqual(shared,0)

    def test_scores_the_frozen_check_start_even_if_usage_changes_during_the_check(self):
        self.observe(11, prediction=None)
        with self.usage.state() as state:
            state['rate'] = 0.001
        self.work(sent=3000)
        self.assertEqual(self.usage.snapshot()['used'], 14)
        self.work(reply=2000)
        after = self.observe(16, prediction=dict(used=14,reset='Nov 13'))
        self.assertEqual(after['observed'],16)
        self.assertEqual(after['accuracy'],dict(count=1,latest_error_pp=-2,recent_mae_tenths=20))
        entry = self.usage.accuracy_report()['checks'][-1]
        self.assertEqual((entry['estimate'],entry['verified'],entry['timing'],entry['error_pp']),(14,16,'check_start',-2))

    def test_legacy_reset_and_missing_predictions_are_separate_from_the_trend(self):
        self.observe(10, prediction=None)
        self.observe(11)
        self.observe(0, 'Nov 20', prediction=dict(used=12,reset='Nov 13'))
        self.observe(1, 'Nov 20', prediction=dict(used=True,reset='Nov 20'))
        report=self.usage.accuracy_report()
        self.assertEqual(report['summary']['scored'],0)
        self.assertEqual((report['checks'][1]['timing'],report['checks'][1]['error_pp']),('before_apply',-1))
        self.assertEqual(report['checks'][2]['estimate'],12)
        self.assertIsNone(report['checks'][2]['error_pp'])
        self.assertIsNone(report['checks'][3]['estimate'])

    def test_retry_does_not_duplicate_history_and_windows_compare_equal_sample_sizes(self):
        for error in (-4, -3, -2, -1, 0, 1, 1, -1, 0, 0):
            args=dict(used=10,prediction=dict(used=10+error,reset='Nov 13'))
            self.observe(advance=3600,**args)
            self.observe(advance=0,**args)
        report=self.usage.accuracy_report()
        self.assertEqual(len(report['checks']),10)
        self.assertEqual(report['summary']['recent'],dict(count=5,mean_absolute_pp=0.6,mean_signed_pp=0.2))
        self.assertEqual(report['summary']['previous'],dict(count=5,mean_absolute_pp=2.0,mean_signed_pp=-2.0))
        for _ in range(ACCURACY_LIMIT):
            self.observe(10,advance=3600,prediction=dict(used=10,reset='Nov 13'))
        self.assertEqual(len(self.usage.accuracy_report()['checks']),ACCURACY_LIMIT)
