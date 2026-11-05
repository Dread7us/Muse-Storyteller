import hashlib
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
import os
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from muse_vm_tts_job import synthesize


@unittest.skipUnless(os.name == "posix", "Linux subprocess streaming test")
class Jobs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.job = self.root / 'job'
        self.job.mkdir()
        (self.job / 'source.md').write_text('**SCP-9192** cost $12.45.', encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def fake(self, body):
        script = self.root / 'tts'
        script.write_text('#!/usr/bin/python3\n' + body)
        script.chmod(0o700)
        return script

    def run_job(self, executable, **kwargs):
        return synthesize(self.job, 'avocado_v2:MAI_03', executable=executable,
                          normalize=lambda value: 'S C P nine one nine two cost twelve dollars.', **kwargs)

    def test_stream_visible_before_completion_and_final_integrity(self):
        executable = self.fake('import sys,time\nsys.stdout.buffer.write(b"a"*1024);sys.stdout.flush();time.sleep(.5);sys.stdout.buffer.write(b"b"*1024)\n')
        errors = []
        def run():
            try:self.run_job(executable)
            except BaseException as error:errors.append(error)
        worker = threading.Thread(target=run)
        worker.start()
        until = time.monotonic()+3
        observed = False
        while time.monotonic()<until:
            try:
                meta = json.loads((self.job/'meta.json').read_text())
                if meta['state']=='streaming':
                    observed=True
                    self.assertTrue(worker.is_alive())
                    self.assertGreater((self.job/'audio.mp3').stat().st_size, 0)
                    break
            except FileNotFoundError:pass
            time.sleep(.01)
        worker.join(4)
        self.assertFalse(worker.is_alive())
        self.assertFalse(errors)
        self.assertTrue(observed)
        meta = json.loads((self.job/'meta.json').read_text())
        self.assertEqual(meta['state'],'complete')
        self.assertEqual(meta['audio_bytes'],2048)
        self.assertEqual(meta['audio_sha256'],hashlib.sha256(b'a'*1024+b'b'*1024).hexdigest())
        with self.assertRaises(FileExistsError):self.run_job(executable)

    def test_failure_is_not_success_even_with_partial_audio(self):
        executable=self.fake('import sys\nsys.stdout.buffer.write(b"x"*256);sys.exit(1)\n')
        with self.assertRaises(RuntimeError):self.run_job(executable)
        self.assertEqual(json.loads((self.job/'meta.json').read_text())['state'],'error')

    def test_timeout_terminates_own_synthesis(self):
        executable=self.fake('import time\ntime.sleep(30)\n')
        start=time.monotonic()
        with self.assertRaises(TimeoutError):self.run_job(executable,deadline_seconds=.15)
        self.assertLess(time.monotonic()-start,4)
        self.assertEqual(json.loads((self.job/'meta.json').read_text())['error'],'timeout')

    def test_bad_voice_and_oversize_source_rejected_before_execution(self):
        with self.assertRaises(ValueError):synthesize(self.job,'avocado_v2:x;touch bad',normalize=str)
        (self.job/'source.md').write_bytes(b'x'*8193)
        with self.assertRaises(ValueError):self.run_job('/not/executed')
        self.assertFalse((self.job/'claimed').exists())
    def test_compact_delivery_keeps_original_and_hashes_complete_output(self):
        executable=self.fake('import sys\nsys.stdout.buffer.write(b"x"*2048)\n')
        encoder=self.root/'encoder'
        encoder.write_text('#!/usr/bin/python3\nfrom pathlib import Path\nimport sys\nPath(sys.argv[-1]).write_bytes(b"c"*1024)\n');encoder.chmod(0o700)
        meta=self.run_job(executable,compact=True,ffmpeg=encoder)
        self.assertEqual((self.job/'original.mp3').read_bytes(),b'x'*2048)
        self.assertEqual((self.job/'audio.mp3').read_bytes(),b'c'*1024)
        self.assertEqual(meta['audio_sha256'],hashlib.sha256(b'c'*1024).hexdigest())
        self.assertEqual(meta['audio_bytes'],1024);self.assertEqual(meta['delivery_sample_rate'],16000)
    def test_failed_compaction_never_publishes_partial_audio_as_complete(self):
        executable=self.fake('import sys\nsys.stdout.buffer.write(b"x"*2048)\n')
        encoder=self.root/'encoder';encoder.write_text('#!/usr/bin/python3\nimport sys\nsys.exit(1)\n');encoder.chmod(0o700)
        with self.assertRaises(RuntimeError):self.run_job(executable,compact=True,ffmpeg=encoder)
        self.assertEqual(json.loads((self.job/'meta.json').read_text())['state'],'error')
        self.assertFalse((self.job/'audio.mp3').exists());self.assertEqual((self.job/'original.mp3').stat().st_size,2048)


if __name__=='__main__':unittest.main()
