add_library(binbot_speexdsp STATIC
    libspeexdsp/mdf.c
    libspeexdsp/preprocess.c
    libspeexdsp/fftwrap.c
    libspeexdsp/filterbank.c
    libspeexdsp/kiss_fft.c
    libspeexdsp/kiss_fftr.c
    libspeexdsp/resample.c
)
target_include_directories(binbot_speexdsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(binbot_speexdsp PRIVATE FLOATING_POINT USE_KISS_FFT VAR_ARRAYS EXPORT=)
target_link_libraries(binbot_speexdsp PUBLIC m)
