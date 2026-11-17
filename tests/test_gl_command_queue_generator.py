"""The maintained generator must reproduce both committed GL wrapper outputs."""
from pathlib import Path
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
class QueueGeneratorTests(unittest.TestCase):
    def test_actual_outputs_are_byte_exact_without_writes(self):
        names=[ROOT/'emulator/src/video_core/renderer_opengl'/n for n in ['gl_command_queue_generated.inc','gl_command_queue_api.h']]
        before=[p.read_bytes() for p in names]
        result=subprocess.run([sys.executable,str(ROOT/'tools/generate_gl_command_queue.py'),'--check'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr+result.stdout)
        self.assertEqual([p.read_bytes() for p in names],before)
        self.assertIn('"byte_exact": true',result.stdout)
if __name__=='__main__':unittest.main()
