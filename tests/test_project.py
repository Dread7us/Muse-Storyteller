import ast
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'module/runtime'))

class ProjectChecks(unittest.TestCase):
    def test_python_sources_parse(self):
        for folder in ('module','tools','integrations'):
            for p in (ROOT/folder).rglob('*.py'): ast.parse(p.read_text(encoding='utf-8-sig'),filename=str(p))

    def test_overlay_integrity(self):
        manifest=json.loads((ROOT/'firmware/upstream.json').read_text())
        self.assertEqual(len(manifest['commit']),40)
        for name,item in manifest['files'].items():
            self.assertNotIn('..',Path(name).parts)
            self.assertEqual(hashlib.sha256((ROOT/'firmware/overlay'/name).read_bytes()).hexdigest(),item['overlay_sha256'],name)

    def test_models_are_external(self):
        assets=json.loads((ROOT/'module/assets.json').read_text())['assets']
        self.assertTrue(any(a['name']=='llm-model-whisper-base' for a in assets))
        for asset in assets:
            self.assertTrue(asset['url'].startswith('https://'))
            self.assertEqual(len(asset['sha256']),64)
            self.assertGreater(asset['bytes'],0)

    def test_explicit_command_boundaries(self):
        compiler=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not compiler: self.skipTest('Native C compiler unavailable; CI runs this check.')
        with tempfile.TemporaryDirectory() as d:
            exe=Path(d)/('commands.exe' if sys.platform=='win32' else 'commands')
            subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/overlay/esp32/components/muse'),str(ROOT/'tests/device_commands.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

class SpeechChecks(unittest.TestCase):
    def test_plain_scores_are_natural(self):
        from muse_speech_text import SpeechText
        text=SpeechText('The score was 35-20.').text.lower()
        self.assertIn('thirty five',text);self.assertIn('twenty',text)

    def test_identifier_digits_and_money(self):
        from muse_speech_text import SpeechText
        text=SpeechText('SCP-9192 paid $12.').text.lower()
        self.assertIn('nine one nine two',text);self.assertIn('twelve dollars',text)

    def test_formatting_not_spoken(self):
        from muse_speech_text import SpeechText
        text=SpeechText('**Hello** and *welcome*.').text
        self.assertNotIn('*',text)

    def test_segments_retain_complete_text(self):
        from muse_narration_plan import sentences
        text='The lamp glowed. The keeper opened the door! Then she said hello.'
        pieces=list(sentences(text))
        self.assertEqual(' '.join(pieces),text)

    def test_asr_silence_not_submitted(self):
        from muse_whisper_asr import Segmenter
        jobs=[];seg=Segmenter(lambda *x:jobs.append(x));seg.begin()
        for _ in range(100):seg.feed(b'\0'*640,-100)
        seg.finish();self.assertEqual(jobs,[])

    def test_asr_final_words_are_flushed(self):
        from muse_whisper_asr import Segmenter
        jobs=[];seg=Segmenter(lambda *x:jobs.append(x));seg.begin()
        for _ in range(10):seg.feed(b'\1\0'*320,-20)
        seg.finish();self.assertEqual(len(jobs),1);self.assertEqual(len(jobs[0][0]),6400)

    def test_overlap_only_removes_repeated_prefix(self):
        from muse_whisper_asr import dedupe
        self.assertEqual(dedupe('the blue lantern','blue lantern is on the table'),'is on the table')
        self.assertEqual(dedupe('the blue lantern','do not turn off the light'),'do not turn off the light')

class RecorderChecks(unittest.TestCase):
    def test_review_progress_completes_and_raw_is_preserved(self):
        import muse_transcript_review as review
        raw='[00:00:01] '+' '.join(['hello']*70)+'\n'
        class Client:
            def __init__(self,check): pass
            def correct(self,text): return text
            def close(self): pass
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d);(folder/'transcript.txt').write_text(raw,encoding='utf8')
            review.review(folder,lambda:True,Client)
            status=review.status(folder)
            self.assertEqual(status['status'],'complete');self.assertEqual(status['chunks'],status['total']);self.assertEqual(status['total'],3)
            self.assertEqual((folder/'transcript.txt').read_text(encoding='utf8'),raw)

    def test_reviewer_does_not_drop_negation_or_change_numbers(self):
        from muse_transcript_review import acceptable
        self.assertFalse(acceptable('do not turn off the light','turn off the light'))
        self.assertFalse(acceptable('I paid twelve dollars','I paid twenty dollars'))

if __name__=='__main__': unittest.main()
