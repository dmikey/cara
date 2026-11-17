"""Audit policy checks only; no executable or compiler is invoked."""
import importlib.util
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('build_mac_sdl_audit',ROOT/'tools/build_mac_sdl.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class NativeAuditTests(unittest.TestCase):
    def run_audit(self,atlas=True,symbol=' _mmap\n',mapping='PROT_READ|PROT_WRITE',missing=False,jit=False,cpu_jit=False,jit_object=True):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);build=root/'build';build.mkdir()
            (build/'bin').mkdir();(build/'bin/cara').write_bytes(b'non-executable policy fixture')
            (build/'CMakeCache.txt').write_text('CARA_MAC_SDL_DEV_HOST:BOOL=ON\nCARA_ENABLE_JIT:BOOL='+('ON' if jit else 'OFF')+'\nCARA_MAC_CPU_JIT:BOOL='+('ON' if cpu_jit else 'OFF')+'\nCARA_MAC_ATLAS_PIPELINE:BOOL='+('ON' if atlas else 'OFF')+'\n')
            names=('gl_host_latency.cpp','gl_pipeline_capture.cpp')
            (build/'build.ninja').write_text('\n'.join(n+'.o' for n in names if not(missing and n==names[0])) + ('\narm/dynarmic/arm_dynarmic.cpp.o' if cpu_jit and jit_object else ''))
            for name in names:
                path=root/'emulator/src/video_core/renderer_opengl'/name;path.parent.mkdir(parents=True,exist_ok=True)
                path.write_text('void* map=mmap(nullptr,sizeof(Journal),'+mapping+',MAP_SHARED,fd,0);')
            def output(command,**kwargs):return symbol if command[0]=='nm' else 'fixture linkage'
            with patch.object(module,'ROOT',root),patch.object(module.subprocess,'check_output',side_effect=output):
                module.audit(build)
                report=json.loads((build/('native-cpu-jit-audit.json' if cpu_jit else 'native-no-jit-audit.json')).read_text())
                self.assertEqual(report['binary_sha256'],hashlib.sha256(b'non-executable policy fixture').hexdigest())
                self.assertEqual(report['jit_enabled'],cpu_jit)
                self.assertFalse(report['shader_jit_enabled'])
    def test_explicit_atlas_rw_file_journals(self):self.run_audit()
    def test_ordinary_build_rejects_mmap(self):
        with self.assertRaises(RuntimeError):self.run_audit(atlas=False)
    def test_execute_mapping_rejected(self):
        with self.assertRaises(RuntimeError):self.run_audit(mapping='PROT_READ|PROT_EXEC')
    def test_missing_object_rejected(self):
        with self.assertRaises(RuntimeError):self.run_audit(missing=True)
    def test_execute_permission_symbol_rejected(self):
        with self.assertRaises(RuntimeError):self.run_audit(symbol=' _mprotect\n')
    def test_jit_symbol_rejected(self):
        with self.assertRaises(RuntimeError):self.run_audit(symbol=' ARM_Dynarmic\n')
    def test_jit_build_rejected(self):
        with self.assertRaises(RuntimeError):self.run_audit(jit=True)
    def test_optional_cpu_jit_with_shader_jit_off(self):
        self.run_audit(cpu_jit=True,symbol=' ARM_Dynarmic\n _mprotect\n _mmap\n')
    def test_cpu_jit_requires_actual_object(self):
        with self.assertRaises(RuntimeError):self.run_audit(cpu_jit=True,jit_object=False,symbol=' ARM_Dynarmic\n')
    def test_cpu_jit_requires_actual_symbol(self):
        with self.assertRaises(RuntimeError):self.run_audit(cpu_jit=True)
    def test_cpu_jit_still_rejects_shader_jit(self):
        with self.assertRaises(RuntimeError):self.run_audit(cpu_jit=True,symbol=' ARM_Dynarmic\n JitX64Engine\n')
if __name__=='__main__':unittest.main()
