#!/Library/Developer/CommandLineTools/usr/bin/python3
"""Local Mac gameplay launcher. No benchmark or developer command channel."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


def digest(path):
    with path.open('rb') as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
        return result.hexdigest()


def environment(user, native='1', native_geometry=False, isolated_dispatch=False, shader_cache='1'):
    if native not in ('0', '1'):
        raise ValueError('Native remap override must be 0 or 1')
    if type(native_geometry) is not bool:
        raise ValueError('Native triangle geometry selection must be boolean')
    if type(isolated_dispatch) is not bool:
        raise ValueError('Isolated shader dispatch selection must be boolean')
    if shader_cache not in ('0', '1'):
        raise ValueError('Shader cache override must be 0 or 1')
    env = {k: v for k, v in os.environ.items() if not k.startswith('CARA_')}
    env.update({
        # The render worker must not synchronously wait for AppKit's main
        # thread while that thread is waiting for the bounded render queue.
        # SDL still schedules the required window/context update on main.
        'SDL_MAC_OPENGL_ASYNC_DISPATCH': '1',
        'CARA_USER_DIR': str(user),
        'CARA_NATIVE_DIAGNOSTICS': '0',
        'CARA_NATIVE_DEV_COMMANDS': '0',
        'CARA_NATIVE_BENCHMARK': '0',
        'CARA_NATIVE_SCRIPT_ONLY_INPUT': '0',
        'CARA_NATIVE_TIMING_COUNTERS': '0',
        'CARA_NATIVE_SHADER_CACHE': shader_cache,
        'CARA_GL_PIPELINE_CAPTURE': '0',
        'CARA_GL_HOST_LATENCY': '0',
        'CARA_NATIVE_LOAD_SLOT': '0',
        'CARA_GL_COMMAND_QUEUE': '1',
        'CARA_GL_QUEUE_NATIVE_MAP_UPLOAD': '1',
        'CARA_GPU_INDEXED_INPUTS': '1',
        'CARA_GPU_PACKED_INPUTS': '1',
        'CARA_GPU_SURFACE_REMAP': '1',
        'CARA_GPU_BYTE_SURFACE_REMAP': '1',
        'CARA_NATIVE_RGBA8_SURFACE_REMAP': native,
        'CARA_NATIVE_TRIANGLE_QUATERNION_GEOMETRY': '1' if native_geometry else '0',
        'CARA_NATIVE_ISOLATED_SHADER_DISPATCH': '1' if isolated_dispatch else '0',
        'CARA_GPU_DEPTH_REINTERPRET': '1',
        'CARA_MAC_GPU_DEPTH_REINTERPRET': '0',
        'CARA_VS_UNIFORM_REUSE': '1',
        'CARA_GL_STATE_REUSE': '1',
        'CARA_VERTEX_UPLOAD_BANK_MIB': '80',
        'CARA_VERTEX_UPLOAD_REUSE': '1',
        'CARA_VERTEX_UPLOAD_ORDER_REUSE': '1',
        'CARA_VERTEX_INDEX_CACHE': '1',
        'CARA_VERTEX_INDEX_IDENTITY_REUSE': '1',
        'CARA_NATIVE_COPY_RUNTIME_QUERIES': '1',
        'CARA_NATIVE_COPY_FORCE_QUERIES': '0',
        'CARA_GL_QUEUE_UNCHECKED_BLIT_PRODUCER': '0',
        'CARA_MAC_VSYNC': '0',
    })
    for key in ('CARA_NATIVE_NEAREST_BORROW', 'CARA_NATIVE_COPYTEX_SURFACE_COPY',
                'CARA_NATIVE_SHADER_SURFACE_COPY', 'CARA_GPU_FEEDBACK_GROW_ONLY',
                'CARA_FASTINTERP_LDR_SP_STATIC', 'CARA_FASTINTERP_LDR_STR_FOUR_PAIRS',
                'CARA_FASTINTERP_STR_SAME_FOUR', 'CARA_FASTINTERP_ADD_CMP_REG',
                'CARA_PICA_SHADER_STREAM_BATCH', 'CARA_PICA_REPEAT_STATE',
                'CARA_OLD3DS_SYSCORE', 'CARA_SW_DEFER_FRAMEBUFFER_FLUSH',
                'CARA_SW_ADJACENT_BATCH', 'CARA_LUT_EFFECTIVE_CHANGE'):
        env[key] = '0'
    return env


def main():
    resources = Path(__file__).resolve().parent
    manifest = json.loads((resources / 'build.json').read_text())
    binary = resources.parent / 'MacOS' / 'cara-core'
    if digest(binary) != manifest['binary_sha256']:
        raise RuntimeError('The gameplay executable differs from the verified build')
    user = Path(manifest['user_dir']).expanduser()
    if not user.is_absolute() or user.is_symlink():
        raise RuntimeError('Invalid gameplay data directory')
    user.mkdir(parents=True, exist_ok=True)
    with (user / '.gameplay.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Cara is already using this game data directory')
        # No default ROM and no automatic state restore. Empty arguments show Welcome.
        arguments = sys.argv[1:]
        if any(arg.startswith('-psn_') for arg in arguments):
            arguments = [arg for arg in arguments if not arg.startswith('-psn_')]
        if len(arguments) > 1 or (arguments and not Path(arguments[0]).is_file()):
            raise RuntimeError('Choose a game with Open Game in Cara')
        env = environment(user, native_geometry=False, isolated_dispatch=True)
        env['CARA_NATIVE_SHADER_SUFFIX_SHARING'] = '0'
        with (user / 'last-launch.log').open('ab') as log:
            return subprocess.call([str(binary)] + arguments, env=env, stdout=log, stderr=log)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        # osascript arguments carry text without evaluating it as AppleScript.
        subprocess.run(['/usr/bin/osascript', '-e',
                        'on run argv\n display alert "Cara" message (item 1 of argv) as critical\nend run',
                        str(error)], check=False)
        sys.exit(1)
