"""Regression tests for bounded-memory ZIP64 corpus packaging."""
import importlib.util
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
            target = root / "corpus.brscanpack"
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
