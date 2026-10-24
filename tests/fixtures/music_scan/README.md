These FLAC and MP3 fixtures contain seven synthetic changing tones at 48 kHz,
mono, for 0.5 seconds. They contain no recorded music and are dedicated to the
public domain (CC0). `generate.py` creates both from the same PCM16 signal using
a supplied FFmpeg executable (generated with FFmpeg 7.1).

The scanner tests decode both formats and check duration, finite nonzero
statistics and native bandwidth. Exact sample equality is not expected for MP3.
No test plays the signal through speakers.
