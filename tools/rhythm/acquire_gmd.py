"""Acquire only prospectively selected GMD development pairs by bounded HTTP ranges.

The frozen protocol selects identities before any metric is read. Holdout and
selection payloads are deliberately inaccessible through this command. Partial
archive access verifies member CRC/length and records SHA256; it cannot verify
the published hash of the whole 5 GB ZIP.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tempfile
import wave
import zipfile
import zlib

URL = 'https://storage.googleapis.com/magentadata/datasets/groove/groove-v1.0.0.zip'
ARCHIVE_BYTES = 5111599714
MAX_RANGE = 32 * 1024 * 1024


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Archive(io.RawIOBase):
    def __init__(self):
        self.position = 0
        self.downloaded = 0

    def seekable(self):
        return True

    def tell(self):
        return self.position

    def seek(self, offset, whence=io.SEEK_SET):
        self.position = (0 if whence == io.SEEK_SET else
                         self.position if whence == io.SEEK_CUR else ARCHIVE_BYTES) + offset
        if not 0 <= self.position <= ARCHIVE_BYTES:
            raise ValueError('Seek outside archive')
        return self.position

    def read(self, size=-1):
        size = ARCHIVE_BYTES - self.position if size < 0 else size
        size = min(size, ARCHIVE_BYTES - self.position)
        if not size:
            return b''
        if not 0 < size <= MAX_RANGE:
            raise ValueError('Refuse unbounded archive download')
        start, end = self.position, self.position + size - 1
        with tempfile.TemporaryDirectory(prefix='gmd-range-') as directory:
            data, headers = Path(directory) / 'range.bin', Path(directory) / 'headers.txt'
            subprocess.run(['curl', '--fail', '--silent', '--show-error', '--max-time', '60',
                            '--range', f'{start}-{end}', '-D', str(headers),
                            '-o', str(data), URL], check=True)
            header = headers.read_text(errors='replace').lower()
            if not re.search(r'http/\S+ 206\b', header):
                raise ValueError('Archive server ignored bounded range')
            if f'content-range: bytes {start}-{end}/{ARCHIVE_BYTES}' not in header:
                raise ValueError('Archive identity/size/range mismatch')
            payload = data.read_bytes()
        if len(payload) != size:
            raise ValueError('Truncated range')
        self.position += size
        self.downloaded += size
        return payload


def member(archive: Archive, info: zipfile.ZipInfo) -> bytes:
    if info.flag_bits or info.compress_type != zipfile.ZIP_DEFLATED:
        raise ValueError('Unexpected member encoding')
    if info.file_size > MAX_RANGE or info.compress_size > MAX_RANGE:
        raise ValueError('Member exceeds acquisition bound')
    archive.seek(info.header_offset)
    header = archive.read(30)
    signature, _, flags, method, _, _, crc, compressed, size, name, extra = struct.unpack('<IHHHHHIIIHH', header)
    if (signature, flags, method, crc, compressed, size) != (
            0x04034b50, 0, 8, info.CRC, info.compress_size, info.file_size):
        raise ValueError('Central/local header mismatch')
    if archive.read(name).decode('utf8') != info.filename:
        raise ValueError('Member name mismatch')
    archive.seek(extra, io.SEEK_CUR)
    data = zlib.decompress(archive.read(info.compress_size), -15)
    if len(data) != info.file_size or zlib.crc32(data) != info.CRC:
        raise ValueError('Member length/CRC mismatch')
    return data


def selected(protocol):
    identities = protocol['initial_development_ids']
    candidates = {row['id']: row for row in protocol['roles']['development']}
    rows = [candidates[identity] for identity in identities]
    if len(set(identities)) != len(identities):
        raise ValueError('Duplicate frozen development identity')
    if any(row['drummer'] in protocol['reserved_performers'] or row['split'] != 'train' for row in rows):
        raise ValueError('Development selection crosses reserved performer/split')
    return rows


def acquire(protocol_path: Path, output: Path):
    encoded = protocol_path.read_bytes()
    protocol = json.loads(encoded)
    rows = selected(protocol)
    output.mkdir(parents=True, exist_ok=False)
    archive = Archive()
    with zipfile.ZipFile(archive) as central:
        info_data = member(archive, central.getinfo('groove/info.csv'))
        if sha(info_data) != protocol['info_sha256']:
            raise ValueError('Official metadata differs from frozen protocol')
        official = {row['id']: row for row in csv.DictReader(io.StringIO(info_data.decode('utf8')))}
        if any(official.get(row['id']) != row for row in rows):
            raise ValueError('Selected metadata/paths differ from the official frozen rows')
        (output / 'info.csv').write_bytes(info_data)
        entries = []
        for row in rows:
            entry = {'role': 'development', 'metadata': row, 'members': []}
            for key in ('audio_filename', 'midi_filename'):
                relative = PurePosixPath('groove') / row[key]
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('Unsafe archive member path')
                info = central.getinfo(str(relative))
                data = member(archive, info)
                destination = output.joinpath(*relative.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with destination.open('xb') as file:
                    file.write(data)
                entry['members'].append({'path': str(relative), 'bytes': len(data),
                                         'sha256': sha(data), 'crc32': f'{info.CRC:08x}'})
                print('VERIFIED', relative, len(data), flush=True)
            with wave.open(str(output / 'groove' / row['audio_filename']), 'rb') as audio:
                entry['audio'] = {'channels': audio.getnchannels(), 'sample_rate': audio.getframerate(),
                                  'sample_width': audio.getsampwidth(), 'samples': audio.getnframes()}
            entries.append(entry)
    manifest = {'schema': 'gmd-development-acquisition.v1', 'protocol_sha256': sha(encoded),
                'source': 'https://magenta.tensorflow.org/datasets/groove', 'archive_url': URL,
                'license': 'CC BY 4.0; Google LLC; Gillick et al., Learning to Groove, ICML 2019',
                'archive_bytes': ARCHIVE_BYTES, 'downloaded_bytes': archive.downloaded,
                'full_archive_sha256_verified': False, 'member_crc_verified': True,
                'info_sha256': sha(info_data), 'entries': entries,
                'holdout_payload_accessed': False}
    (output / 'acquisition.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    arguments = parser.parse_args()
    acquire(arguments.protocol, arguments.out)
