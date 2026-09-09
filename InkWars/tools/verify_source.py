#!/usr/bin/env python3
"""Rebuild the corresponding-source archive in isolation and compare target bytes."""
from pathlib import Path
import tempfile, tarfile, subprocess, hashlib, re
ROOT=Path(__file__).resolve().parents[1]
VERSION=re.search(r'#define IW_VERSION "([0-9.]+)"', (ROOT/'src/version.h').read_text()).group(1)
archive=ROOT/f'build/inkwars-source-{VERSION}.tar.gz'
with tempfile.TemporaryDirectory(prefix='inkwars-source-') as directory:
 with tarfile.open(archive,'r:gz') as tar:
  for member in tar.getmembers():
   if not member.isfile() or member.name.startswith('/') or '..' in Path(member.name).parts:raise ValueError('Unsafe source member')
  tar.extractall(directory)
 project=Path(directory)/'InkWars'
 subprocess.run(['make','host','test','target','verify'],cwd=project,check=True)
 rebuilt=(project/'build/inkwars').read_bytes();original=(ROOT/'build/inkwars').read_bytes()
 if rebuilt!=original:raise SystemExit('Source rebuild differs from release executable')
 print('PASS isolated source archive builds all tests and reproduces target SHA256 '+hashlib.sha256(rebuilt).hexdigest())
