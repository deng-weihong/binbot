SpeexDSP 1.2.1, Xiph.Org Foundation (BSD license, see COPYING).

Source: https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz
SHA256: 8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d

The upstream DSP sources and headers are unmodified. BinBot adds a CMake target
for the echo canceller, residual echo suppression, and playback resampler, and a
stdint-based types header in place of the Autoconf-generated header. Host tests
use the floating-point echo canceller. The RV1106 uses the Rockchip SDK for echo
cancellation and links only SpeexDSP's resampler for the playback reference.
