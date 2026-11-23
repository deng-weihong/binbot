add_executable(binbot_wake src/main.cc)

target_link_libraries(binbot_wake PRIVATE
    binbot_audio_ipc
    binbot_nihaomiya_kws
    m
    Threads::Threads
)

if(TARGET binbot_snowboy_kws)
    target_link_libraries(binbot_wake PRIVATE binbot_snowboy_kws)
    target_compile_definitions(binbot_wake PRIVATE BINBOT_HAS_SNOWBOY=1)
endif()

if(TARGET binbot_hixiaowen_kws)
    target_link_libraries(binbot_wake PRIVATE binbot_hixiaowen_kws)
    target_compile_definitions(binbot_wake PRIVATE BINBOT_HAS_HIXIAOWEN=1)
endif()

target_compile_options(binbot_wake PRIVATE -Wall -Wextra -Wpedantic)
