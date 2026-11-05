import os,subprocess,sys,tempfile,unittest
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

class Chime(unittest.TestCase):
 @unittest.skipUnless(sys.platform.startswith('linux'),'Linux boot helper')
 def test_simultaneous_startup_calls_play_only_once(self):
  source=Path(__file__).resolve().parents[1]/'module/runtime/muse_boot_chime.sh'
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);events=root/'calls';player=root/'tinyplay';helper=root/'chime.sh'
   player.write_text('#!/bin/sh\nprintf "played\\n" >> "'+str(events)+'"\n');player.chmod(0o700)
   helper.write_text(source.read_text().replace('/run/muse-startup-chime.once',str(root/'once')).replace('exec tinyplay','exec "'+str(player)+'"'))
   with ThreadPoolExecutor(max_workers=8) as pool:
    jobs=[pool.submit(subprocess.run,['/bin/sh',str(helper)],check=True) for _ in range(12)]
    for job in jobs:job.result()
   self.assertEqual(events.read_text(),'played\n')
   self.assertEqual(os.stat(root/'once').st_mode & 0o170000,0o040000)

if __name__=='__main__':unittest.main()
