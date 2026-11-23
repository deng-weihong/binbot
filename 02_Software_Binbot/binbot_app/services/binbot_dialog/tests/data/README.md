# Quiet Speech Regression

`quiet-speech-16k.pcm` contains three seconds of generated Chinese test speech,
16 kHz mono signed 16-bit little-endian PCM, normalized to RMS 400.
It is an excerpt of "这是一段语音识别测试，请回复测试成功。" generated for BinBot verification.

The regression mixes deterministic noise at about RMS 580. Most frames fall
below the previous device amplitude gate of approximately 850. It verifies
that quiet input reaches final recognition when streaming results are delayed,
and that new streaming text keeps an active utterance from being cut off.
