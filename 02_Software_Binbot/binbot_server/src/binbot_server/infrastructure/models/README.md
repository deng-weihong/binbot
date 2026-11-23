# Silero VAD model

`silero_vad.onnx` is the unmodified model from Silero VAD v5.1.2:
https://github.com/snakers4/silero-vad/tree/v5.1.2

Source: `src/silero_vad/data/silero_vad.onnx`.
License: MIT, reproduced in `silero_vad.LICENSE`.
SHA-256: `2623a2953f6ff3d2c1e61740c6cdb7168133479b267dfef114a4a3cc5bdd788f`.

BinBot runs the 16 kHz model with 512-sample windows, a 64-sample context,
and independent recurrent state for each device connection. The ONNX inference
session is shared; connection audio and state are never shared.
