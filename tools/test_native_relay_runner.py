#!/usr/bin/env python3
"""ROM-free checks that the relay launcher preserves existing user settings."""
from pathlib import Path
from tempfile import TemporaryDirectory
from run_native_relay import prepare_gpu_config

with TemporaryDirectory() as directory:
    config=Path(directory)/'config/sdl2-config.ini'
    try:prepare_gpu_config(directory)
    except ValueError:pass
    else:raise AssertionError('Missing configuration was silently accepted')
    assert not config.exists()
    prepare_gpu_config(directory,True)
    original=config.read_bytes();prepare_gpu_config(directory,True);assert config.read_bytes()==original
    custom=b'[Renderer]\nuse_hw_renderer=false\nuse_hw_shader=true\n[Other]\nkeep=original\n'
    config.write_bytes(custom)
    try:prepare_gpu_config(directory,True)
    except ValueError:pass
    else:raise AssertionError('Software fallback was silently accepted')
    assert config.read_bytes()==custom
    config.write_text('[Renderer]\nuse_hw_renderer=true\nuse_hw_shader=true\n[Other]\nkeep=original\n')
    custom=config.read_bytes();prepare_gpu_config(directory);assert config.read_bytes()==custom
    config.write_text('invalid ini')
    try:prepare_gpu_config(directory,True)
    except ValueError:pass
    else:raise AssertionError('Malformed configuration was accepted')
    assert config.read_text()=='invalid ini'
print('PASS native relay launcher explicit GPU preset and preservation')
