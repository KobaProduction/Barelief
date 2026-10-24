"""Regenerate the CC0 seven-tone fixtures using a supplied FFmpeg executable."""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave


def generate(executable: str) -> None:
    destination = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "seven-changing-tones.wav"
        with wave.open(str(source), "wb") as audio:
            audio.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            for index in range(24000):
                time = index / 48000
                value = sum(0.025 * math.sin(2 * math.pi * ((350 + 450 * tone) * time + 70 * time * time)) for tone in range(7))
                audio.writeframesraw(struct.pack("<h", round(value * 32767)))
        for suffix, options in (("flac", ["-c:a", "flac"]), ("mp3", ["-c:a", "libmp3lame", "-b:a", "128k"])):
            subprocess.run([executable, "-nostdin", "-v", "error", "-y", "-i", str(source), *options,
                            str(destination / ("seven-changing-tones." + suffix))], check=True)


if __name__ == "__main__":
    generate(sys.argv[1])
