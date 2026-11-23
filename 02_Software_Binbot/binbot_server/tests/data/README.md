# Voice endpoint fixture

`quiet-speech-16k.pcm` is synthesized Chinese speech, not a user recording.
It is copied from the BinBot device VAD regression fixture: 16 kHz, mono,
signed 16-bit little-endian PCM, three seconds, RMS approximately 400.

Tests mix deterministic background noise with RMS approximately 580, insert a
1.2-second sentence pause, and reduce speech amplitude to exercise quiet input.
