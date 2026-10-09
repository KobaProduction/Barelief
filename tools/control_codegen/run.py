"""Explicit offline nanopb runner. Invoke with Python -S; never from CMake."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parent.parent

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-tools', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if not sys.flags.no_site:
        parser.error('invoke this runner with Python -S')
    if not args.check_tools and args.output_dir is None:
        parser.error('an explicit --output-dir is required')
    manifest = json.loads((BASE / 'manifest.json').read_text(encoding='utf-8-sig'))
    if list(sys.version_info[:3]) != manifest['python']['version']:
        raise RuntimeError('Python version differs from the verified toolchain')
    if digest(Path(sys.executable)) != manifest['python']['executable_sha256']:
        raise RuntimeError('Python executable hash differs from the verified toolchain')
    for entry in manifest['files']:
        if digest(ROOT / entry['path']) != entry['sha256']:
            raise RuntimeError('Pinned file changed: ' + entry['path'])
    protoc = BASE / 'toolchain/protoc-36.2/bin/protoc.exe'
    if subprocess.check_output([str(protoc), '--version'], text=True).strip() != 'libprotoc 36.2':
        raise RuntimeError('Unexpected protoc version')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(BASE / 'toolchain/python-deps'))
    vendor = ROOT / 'libs/bdsp_protocol/vendor/nanopb/generator'
    sys.path.insert(0, str(vendor))
    import google.protobuf
    if google.protobuf.__version__ != '7.36.2':
        raise RuntimeError('Unexpected Python protobuf version')
    os.environ['PATH'] = str(protoc.parent)
    os.environ.pop('NANOPB_PB2_NO_REBUILD', None)
    schema = ROOT / 'libs/bdsp_protocol/schema'
    with tempfile.TemporaryDirectory(prefix='control-codegen-') as scratch:
        os.environ['NANOPB_PB2_TEMP_DIR'] = scratch
        if args.check_tools:
            argv = ['--version']
        else:
            args.output_dir.resolve().mkdir(parents=True, exist_ok=True)
            argv = ['--error-on-unmatched', '-I', str(schema), '-f',
                    str(schema / 'control.options'), '-D', str(args.output_dir.resolve()),
                    str(schema / 'control.proto')]
        sys.argv = [str(vendor / 'nanopb_generator.py'), *argv]
        runpy.run_path(sys.argv[0], run_name='__main__')

if __name__ == '__main__':
    main()
