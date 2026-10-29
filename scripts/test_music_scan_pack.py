"""Regression tests for bounded-memory ZIP64 corpus packaging."""
import importlib.util
import hashlib
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

MODULE = Path(__file__).resolve().parent / "music_scan_pack.py"
spec = importlib.util.spec_from_file_location("music_scan_pack", MODULE)
packmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packmod)


class MusicScanPackTests(unittest.TestCase):
    def test_pack_verify_extract_and_reject_existing_target(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            collection = root / "collection"
            collection.mkdir()
            parts = []
            for i, n in enumerate((3, 1, 2), 1):
                name = f"music-scan-part-{i:06}.brscan"
                data = {"document": {"analysis_run": {
                    "requested_count": n, "processed_count": n,
                    "failed_count": 0},
                    "compositions": [{"source": f"file_{i}_{k}"} for k in range(n)]}}
                with zipfile.ZipFile(collection / name, "w") as inner:
                    inner.writestr("manifest.json", json.dumps(data))
                    inner.writestr("spectra/sample.bin", b"\x00" * (100 * n))
                parts.append({"name": name, "requested_count": n,
                              "processed_count": n, "failed_count": 0})
            (collection / "manifest.json").write_text(json.dumps({
                "kind": "barelief.music_scan_collection", "version": 1,
                "parts": parts, "requested_count": 6, "processed_count": 6,
                "failed_count": 0}), encoding="utf-8")
            target = root / "corpus.brscan"
            packmod.pack(collection, target)
            self.assertTrue(target.exists())
            packmod.inspect(target)
            packmod.verify(target)
            self.assertFalse(list(root.glob("*.partial")))
            output = root / "second.brscan"
            packmod.extract(target, 2, output)
            self.assertEqual(output.read_bytes(),
                             (collection / parts[1]["name"]).read_bytes())
            with self.assertRaises(FileExistsError):
                packmod.pack(collection, target)
            self.assertTrue((collection / parts[0]["name"]).is_file())


    def test_legacy_wrap_is_lossless_and_single_format(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "legacy.brscan"
            body = {
                "kind": "barelief.music_archive", "content": "scan",
                "document": {
                    "metadata": {"fft_size": 4096},
                    "analysis_run": {"requested_count": 2,
                                     "processed_count": 2, "failed_count": 0},
                    "compositions": [{"source": "a"}, {"source": "b"}],
                },
            }
            with zipfile.ZipFile(source, "w") as inner:
                inner.writestr("manifest.json", json.dumps(body))
                inner.writestr("raw.bin", b"measurement" * 100)
            target = root / "unified.brscan"
            packmod.wrap_legacy(source, target)
            packmod.verify(target)
            with zipfile.ZipFile(target) as outer:
                nested = outer.read("parts/music-scan-part-000001.brscan")
            self.assertEqual(nested, source.read_bytes())
            self.assertTrue(source.exists())
            with zipfile.ZipFile(target) as z:
                cache = {"kind": "barelief.music_scan_summary", "version": 1,
                         "fft_size": 4096, "sample_rate_hz": 48000,
                         "part_count": 1, "processed_count": 2,
                         "manifest_sha256": hashlib.sha256(z.read("manifest.json")).hexdigest(),
                         "index_sha256": hashlib.sha256(z.read("pack-index.json")).hexdigest(),
                         "peak_spectrum": [0.] * 2049,
                         "mean_spectrum": [0.] * 2049,
                         "frequency_presence": [0.] * 2049,
                         "contributors": [2.] * 2049}
            summary = root / "summary.json"
            summary.write_text(json.dumps(cache), encoding="utf-8")
            improved = root / "improved.brscan"
            packmod.add_summary(target, summary, improved)
            packmod.verify(improved)
            self.assertEqual(target.stat().st_size, (root / "unified.brscan").stat().st_size)
            with zipfile.ZipFile(improved) as z:
                self.assertEqual(z.read("parts/music-scan-part-000001.brscan"), source.read_bytes())
            cache["index_sha256"] = "0" * 64
            summary.write_text(json.dumps(cache), encoding="utf-8")
            with self.assertRaises(ValueError):
                packmod.add_summary(target, summary, root / "invalid.brscan")

    def test_incomplete_manifest_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "incomplete"
            source.mkdir()
            (source / "manifest.json").write_text(
                json.dumps({"kind": "barelief.music_scan_collection",
                            "version": 1, "parts": [
                                {"name": "music-scan-part-000001.brscan"}]}),
                encoding="utf-8")
            with self.assertRaises(ValueError):
                packmod.pack(source, Path(td) / "invalid.brscanpack")


if __name__ == "__main__":
    unittest.main()
