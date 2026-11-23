"""Private FastInterp-only PGO policy, including real isolated CMake recipes."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_metal_pgo_builder', ROOT/'tools/build_native_metal.py')
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class NativeMetalPgoTests(unittest.TestCase):
    def test_explicit_default_and_requested_options_on_both_platforms(self):
        for platform in ('mac', 'ios'):
            for generate, use in ((False, None), (True, None), (False, Path('/profile with space.profdata'))):
                args = argparse.Namespace(platform=platform, cpu_jit=False, lto=True,
                    prefix_path=Path('/prefix'), fastinterp_pgo_generate=generate, fastinterp_pgo_use=use)
                command = builder.configure_args(args, Path('/build'), (Path('/glslang'), Path('/spirv')), 'cmake', 'ninja')
                self.assertIn('-DCARA_FASTINTERP_PGO_GENERATE='+('ON' if generate else 'OFF'), command)
                self.assertIn('-DCARA_FASTINTERP_PGO_USE:FILEPATH='+(str(use) if use else ''), command)

    def audit(self, build, generate=False, use=None, fi='', metal='', link='', global_generate=False, kind='frontend', profile_kind=None):
        use=use.resolve() if use else None
        (build/'CMakeCache.txt').write_text(
            f'CARA_FASTINTERP_PGO_GENERATE:BOOL={"ON" if generate else "OFF"}\n'
            f'CARA_FASTINTERP_PGO_USE:FILEPATH={use or ""}\n'
            f'CARA_FASTINTERP_PGO_KIND:STRING={kind}\n'
            f'CARA_PGO_GENERATE:BOOL={"ON" if global_generate else "OFF"}\n')
        with patch.object(builder,'profile_instrumentation_kind',return_value=profile_kind or kind):
            return builder.audit_fastinterp_pgo(build, {'fastinterp': {'flags':fi, 'output':'fastinterp.cpp.o'},
                'metal_bindings': {'flags':metal}}, {'flags':link}, generate, use, kind)

    def test_ir_kind_requires_ir_flags_and_rejects_frontend_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            build=Path(folder)
            report=self.audit(build,True,kind='ir',fi='-fprofile-generate',link='-fprofile-generate')
            self.assertEqual(report['kind'],'ir')
            with self.assertRaisesRegex(RuntimeError,'compile/link runtime'):
                self.audit(build,True,kind='ir',fi='-fprofile-instr-generate',link='-fprofile-instr-generate')
            profile=(build/'ir.profdata').resolve();profile.write_bytes(b'profile fixture')
            flags=f'-fprofile-use={profile} -Werror=profile-instr-out-of-date -Werror=profile-instr-unprofiled'
            self.assertEqual(self.audit(build,use=profile,kind='ir',fi=flags)['profile_kind_verified'],'ir')
            with self.assertRaisesRegex(RuntimeError,'instrumentation kind differs'):
                self.audit(build,use=profile,kind='ir',fi=flags,profile_kind='frontend')

    def test_generation_requires_both_compile_and_transitive_runtime_link(self):
        with tempfile.TemporaryDirectory() as folder:
            build=Path(folder)
            for fi, link in (('', '-fprofile-instr-generate'), ('-fprofile-instr-generate', '')):
                with self.assertRaisesRegex(RuntimeError, 'compile/link runtime'):
                    self.audit(build, True, fi=fi, link=link)
            report=self.audit(build, True, fi='-fprofile-instr-generate', link='-fprofile-instr-generate')
            self.assertEqual(report['mode'], 'generate')
            self.assertTrue(report['generation_runtime_linked'])
            self.assertIsNone(report['profile_path'])

    def test_profile_path_digest_and_exact_matching_diagnostics(self):
        with tempfile.TemporaryDirectory() as folder:
            build=Path(folder); profile=(build/'training with space.profdata').resolve();profile.write_bytes(b'authored profile fixture')
            flags=f'"-fprofile-instr-use={profile}" -Werror=profile-instr-out-of-date -Werror=profile-instr-unprofiled'
            report=self.audit(build,use=profile,fi=flags)
            self.assertEqual(report['mode'],'use')
            self.assertEqual(report['profile_sha256'],hashlib.sha256(profile.read_bytes()).hexdigest())
            with self.assertRaisesRegex(RuntimeError,'exact profile'):
                self.audit(build,use=profile,fi=f'"-fprofile-instr-use={profile}"')

    def test_no_metal_instrumentation_global_mixing_or_stale_flags(self):
        with tempfile.TemporaryDirectory() as folder:
            build=Path(folder)
            with self.assertRaisesRegex(RuntimeError,'Metal bridge'):
                self.audit(build,metal='-fprofile-instr-generate')
            with self.assertRaisesRegex(RuntimeError,'whole-engine'):
                self.audit(build,True,fi='-fprofile-instr-generate',link='-fprofile-instr-generate',global_generate=True)
            with self.assertRaisesRegex(RuntimeError,'compile/link runtime'):
                self.audit(build,fi='-fprofile-instr-generate')
            self.assertEqual(self.audit(build)['mode'],'off')

    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake/Ninja needed')
    def test_real_cmake_source_scope_runtime_propagation_and_reversible_flags(self):
        # Execute the actual production policy in a tiny project, avoiding an
        # engine configure/build. The synthetic files contain no guest code.
        text=(ROOT/'emulator/src/core/CMakeLists.txt').read_text()
        policy=text[:text.index('if(CARA_FASTINTERP_ABSOLUTE_PC)')]
        runtime=text[text.index('if(CARA_FASTINTERP_PGO_GENERATE)\n    # core is static:'):]
        runtime=runtime[:runtime.index('endif()')+len('endif()')]
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); source=root/'arm/fastinterp';source.mkdir(parents=True)
            (source/'fastinterp.cpp').write_text('int authored(int x) {return x > 3 ? x*2 : x+1;}\n')
            (root/'other.cpp').write_text('int other() {return 1;}\n')
            (root/'main.cpp').write_text('int authored(int); int main(){return authored(2)==3?0:1;}\n')
            (root/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(FiPgo LANGUAGES CXX)\n'+policy+
                '\nadd_library(core STATIC arm/fastinterp/fastinterp.cpp other.cpp)\n'+runtime+
                '\nadd_executable(probe main.cpp)\ntarget_link_libraries(probe PRIVATE core)\n')
            build=root/'build'
            def configure(*flags, okay=True):
                result=subprocess.run(['cmake','-S',str(root),'-B',str(build),'-G','Ninja',
                    '-DCMAKE_CXX_COMPILER=clang++',*flags],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode==0,okay,result.stdout)
                return (build/'build.ninja').read_text() if okay else result.stdout
            graph=configure('-DCARA_FASTINTERP_PGO_GENERATE=ON')
            self.assertIn('-fprofile-instr-generate',builder.ninja_build_fields(graph,'CMakeFiles/core.dir/arm/fastinterp/fastinterp.cpp.o')['FLAGS'])
            self.assertNotIn('-fprofile',builder.ninja_build_fields(graph,'CMakeFiles/core.dir/other.cpp.o').get('FLAGS',''))
            self.assertIn('-fprofile-instr-generate',builder.ninja_build_fields(graph,'probe')['LINK_FLAGS'])
            configure('-DCARA_PGO_GENERATE=ON',okay=False)
            graph=configure('-DCARA_PGO_GENERATE=OFF','-DCARA_FASTINTERP_PGO_GENERATE=OFF')
            self.assertNotIn('-fprofile',graph)
            graph=configure('-DCARA_FASTINTERP_PGO_KIND=ir','-DCARA_FASTINTERP_PGO_GENERATE=ON')
            self.assertIn('-fprofile-generate',builder.ninja_build_fields(graph,'CMakeFiles/core.dir/arm/fastinterp/fastinterp.cpp.o')['FLAGS'])
            self.assertIn('-fprofile-generate',builder.ninja_build_fields(graph,'probe')['LINK_FLAGS'])
            self.assertNotIn('-fprofile',builder.ninja_build_fields(graph,'CMakeFiles/core.dir/other.cpp.o').get('FLAGS',''))
            configure('-DCARA_FASTINTERP_PGO_KIND=frontend','-DCARA_FASTINTERP_PGO_GENERATE=OFF')
            configure('-DCARA_FASTINTERP_PGO_USE=/missing/profile',okay=False)


if __name__=='__main__':
    unittest.main()
