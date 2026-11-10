import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'module/runtime'))
from muse_usage import Usage


class PairedUsage(unittest.TestCase):
    def test_recent_observation_applies_and_bad_values_keep_the_anchor(self):
        with tempfile.TemporaryDirectory() as directory:
            usage = Usage(directory)
            now = int(time.time())
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


if __name__ == '__main__':
    unittest.main()
