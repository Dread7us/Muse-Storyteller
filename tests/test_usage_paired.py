import sys
from datetime import datetime, timezone
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'module/runtime'))
from muse_usage import Usage


class PairedUsage(unittest.TestCase):
    def test_recent_observation_applies_and_bad_values_keep_the_anchor(self):
        now = int(datetime(2026, 11, 12, 18, 0, tzinfo=timezone.utc).timestamp())
        with tempfile.TemporaryDirectory() as directory, patch('muse_usage.time.time', return_value=now):
            usage = Usage(directory)
            good = dict(used=8, reset='Nov 13', observed_at=now)
            reading, label = usage.calibrate_paired(good)
            self.assertEqual((reading['observed'], reading['reset']), (8, 'Nov 13'))
            self.assertRegex(label, r'^[A-Za-z]{3} [0-9]{1,2}, 20[0-9]{2} [0-9:]+ (PST|PDT|UTC)$')
            self.assertEqual(usage.calibrate_paired(good)[0]['checked'], reading['checked'])
            for bad in (dict(good, used=True), dict(good, used=101), dict(good, reset='Feb 30'),
                        dict(good, observed_at=now-601), dict(good, observed_at=now+121),
                        dict(good, observed_at=now-10), dict(good, used=9)):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    usage.calibrate_paired(bad)
            self.assertEqual(usage.snapshot()['observed'], 8)

    def test_unset_module_clock_keeps_the_request_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            usage = Usage(directory)
            now = int(time.time())
            with patch('muse_usage.time.time', return_value=1692653709):
                reading, _ = usage.calibrate_paired(dict(used=8, reset='Nov 13', observed_at=now))
                self.assertEqual(reading['observed'], 8)
                with self.assertRaises(ValueError):
                    usage.calibrate_paired(dict(used=6, reset='Nov 13', observed_at=now-1))

    def test_exact_reset_counts_down_without_changing_usage_at_expiry(self):
        with tempfile.TemporaryDirectory() as directory:
            usage = Usage(directory)
            reset_at = int(datetime(2026, 11, 13, 2, 49, tzinfo=timezone.utc).timestamp())
            observed_at = reset_at - 3600
            with patch('muse_usage.time.monotonic', return_value=100):
                reading = usage.calibrate(dict(used=12, reset='Nov 13', checked='Nov 13 01:49',
                                               observed_at=observed_at, reset_at=reset_at))
            self.assertEqual((reading['reset_at'], reading['reset_after_s']), (reset_at, 3600))
            with patch('muse_usage.time.monotonic', return_value=190), patch('muse_usage.time.time', return_value=0):
                self.assertEqual(usage.snapshot()['reset_after_s'], 3510)
            with patch('muse_usage.time.monotonic', return_value=3701):
                expired = usage.snapshot()
            self.assertEqual((expired['reset_after_s'], expired['observed']), (0, 12))
            with patch.object(usage, 'boot', return_value='different boot'):
                with patch('muse_usage.time.time', return_value=observed_at + 180):
                    self.assertEqual(usage.snapshot()['reset_after_s'], 3420)
                with patch('muse_usage.time.time', return_value=0):
                    self.assertEqual(usage.snapshot()['reset_after_s'], -1)

    def test_invalid_reset_metadata_preserves_the_verified_anchor(self):
        with tempfile.TemporaryDirectory() as directory:
            usage = Usage(directory)
            reset_at = int(datetime(2026, 11, 13, 2, 49, tzinfo=timezone.utc).timestamp())
            good = dict(used=12, reset='Nov 13', checked='Nov 10 12:00',
                        observed_at=reset_at-3600, reset_at=reset_at)
            usage.calibrate(good)
            for value in (True, float(reset_at), reset_at-7200, reset_at+9*86400, reset_at+86400):
                with self.subTest(reset_at=value), self.assertRaises(ValueError):
                    usage.calibrate(dict(good, observed_at=good['observed_at']+10, reset_at=value))
            with self.assertRaises(ValueError):
                usage.calibrate(dict(good, reset_at=reset_at+60))
            self.assertEqual(usage.snapshot()['reset_at'], reset_at)
            legacy = usage.calibrate(dict(good, observed_at=good['observed_at']+60, reset_at=None))
            self.assertEqual(legacy['reset_at'], reset_at)
            new_week = usage.calibrate(dict(good, used=0, reset='Nov 20',
                                             observed_at=reset_at+60, reset_at=None))
            self.assertEqual((new_week['reset_at'], new_week['reset_after_s']), (0, -1))


if __name__ == '__main__':
    unittest.main()
