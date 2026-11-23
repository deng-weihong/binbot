set(_BINBOT_ROCKIVA_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING)
    set(_BINBOT_ROCKIVA_DEFAULT ON)
endif()
option(BINBOT_ENABLE_ROCKIVA "Enable the RV1106 RockIVA face detector" ${_BINBOT_ROCKIVA_DEFAULT})

add_library(binbot_vision STATIC
    src/cup_overlay.c
    src/cup_postprocess.c
    src/face_overlay.c
    src/face_matcher.c
    src/hand_gesture.c
    src/hand_overlay.c
)
target_include_directories(binbot_vision
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Keep the RKNN include/library paths shared by the hand, cup, and face
# adapters. This also makes a single-detector cross build work when the other
# RKNN features are disabled.
set(BINBOT_RKNN_INCLUDE_DIR
    "${RV1106_SDK_ROOT}/project/app/rk_smart_door/smart_door/common/face/algo"
    CACHE PATH "RV1106 RKNN runtime include directory"
)
set(BINBOT_RKNN_LIBRARY_DIR
    "${RV1106_SDK_ROOT}/output/out/media_out/lib"
    CACHE PATH "RV1106 RKNN runtime library directory"
)

if(BINBOT_ENABLE_ROCKIVA)
    set(BINBOT_ROCKIVA_SDK_ROOT
        "${RV1106_SDK_ROOT}/media/iva/iva/librockiva/rockiva-rv1106-Linux"
        CACHE PATH "RV1106 RockIVA SDK root"
    )
    if(NOT EXISTS "${BINBOT_ROCKIVA_SDK_ROOT}/include/rockiva_det_api.h")
        message(FATAL_ERROR "RV1106 RockIVA headers not found at ${BINBOT_ROCKIVA_SDK_ROOT}")
    endif()
    target_sources(binbot_vision PRIVATE src/face_detector_rockiva.c)
    target_include_directories(binbot_vision PRIVATE "${BINBOT_ROCKIVA_SDK_ROOT}/include")
    target_link_libraries(binbot_vision PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(binbot_vision PRIVATE BINBOT_HAVE_ROCKIVA=1)
else()
    target_sources(binbot_vision PRIVATE src/face_detector_stub.c)
endif()

set(_BINBOT_RKNN_HAND_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING)
    set(_BINBOT_RKNN_HAND_DEFAULT ON)
endif()
option(BINBOT_ENABLE_RKNN_HAND "Enable RV1106 RKNN hand detection"
       ${_BINBOT_RKNN_HAND_DEFAULT})
if(BINBOT_ENABLE_RKNN_HAND)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_ENABLE_RKNN_HAND requires an RV1106 cross build")
    endif()
    if(NOT EXISTS "${BINBOT_RKNN_INCLUDE_DIR}/rknn_api.h" OR
       NOT EXISTS "${BINBOT_RKNN_LIBRARY_DIR}/librknnmrt.so")
        message(FATAL_ERROR "RV1106 RKNN runtime artifacts are missing")
    endif()
    target_sources(binbot_vision PRIVATE src/hand_detector_rknn.c)
    target_include_directories(binbot_vision PRIVATE "${BINBOT_RKNN_INCLUDE_DIR}")
    target_link_directories(binbot_vision PUBLIC "${BINBOT_RKNN_LIBRARY_DIR}")
    target_link_libraries(binbot_vision PUBLIC rknnmrt Threads::Threads)
    target_link_options(binbot_vision INTERFACE
        "-Wl,-rpath-link,${BINBOT_RKNN_LIBRARY_DIR}"
    )
    add_executable(binbot_hand_detector_probe
        "${PROJECT_SOURCE_DIR}/tools/hand_detector_probe.c"
    )
    target_link_libraries(binbot_hand_detector_probe PRIVATE binbot_vision)
    target_compile_options(binbot_hand_detector_probe PRIVATE
        -Wall -Wextra -Wpedantic
    )
else()
    target_sources(binbot_vision PRIVATE src/hand_detector_stub.c)
endif()

set(_BINBOT_RKNN_CUP_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING)
    set(_BINBOT_RKNN_CUP_DEFAULT ON)
endif()
option(BINBOT_ENABLE_RKNN_CUP "Enable RV1106 RKNN cup detection"
       ${_BINBOT_RKNN_CUP_DEFAULT})
if(BINBOT_ENABLE_RKNN_CUP)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_ENABLE_RKNN_CUP requires an RV1106 cross build")
    endif()
    if(NOT EXISTS "${BINBOT_RKNN_INCLUDE_DIR}/rknn_api.h" OR
       NOT EXISTS "${BINBOT_RKNN_LIBRARY_DIR}/librknnmrt.so")
        message(FATAL_ERROR "RV1106 RKNN runtime artifacts are missing for cup detection")
    endif()
    target_sources(binbot_vision PRIVATE src/cup_detector_rknn.c)
    target_include_directories(binbot_vision PRIVATE "${BINBOT_RKNN_INCLUDE_DIR}")
    target_link_directories(binbot_vision PUBLIC "${BINBOT_RKNN_LIBRARY_DIR}")
    target_link_libraries(binbot_vision PUBLIC rknnmrt Threads::Threads)
    target_link_options(binbot_vision INTERFACE
        "-Wl,-rpath-link,${BINBOT_RKNN_LIBRARY_DIR}"
    )
    add_executable(binbot_cup_detector_probe
        "${PROJECT_SOURCE_DIR}/tools/cup_detector_probe.c"
    )
    target_link_libraries(binbot_cup_detector_probe PRIVATE binbot_vision)
    target_compile_options(binbot_cup_detector_probe PRIVATE
        -Wall -Wextra -Wpedantic
    )
else()
    target_sources(binbot_vision PRIVATE src/cup_detector_stub.c)
endif()

set(_BINBOT_RKNN_FACE_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING)
    set(_BINBOT_RKNN_FACE_DEFAULT ON)
endif()
option(BINBOT_ENABLE_RKNN_FACE "Enable RV1106 RKNN fixed-face recognition"
       ${_BINBOT_RKNN_FACE_DEFAULT})
if(BINBOT_ENABLE_RKNN_FACE)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_ENABLE_RKNN_FACE requires an RV1106 cross build")
    endif()
    if(NOT EXISTS "${BINBOT_RKNN_INCLUDE_DIR}/rknn_api.h" OR
       NOT EXISTS "${BINBOT_RKNN_LIBRARY_DIR}/librknnmrt.so")
        message(FATAL_ERROR "RV1106 RKNN runtime artifacts are missing for face recognition")
    endif()
    target_sources(binbot_vision PRIVATE
        src/face_embedding_rknn.c
        src/face_recognizer_rknn.c
    )
    target_compile_definitions(binbot_vision PRIVATE BINBOT_HAVE_RKNN_FACE=1)
    target_include_directories(binbot_vision PRIVATE "${BINBOT_RKNN_INCLUDE_DIR}")
    target_link_directories(binbot_vision PUBLIC "${BINBOT_RKNN_LIBRARY_DIR}")
    target_link_libraries(binbot_vision PUBLIC rknnmrt Threads::Threads)
    target_link_options(binbot_vision INTERFACE
        "-Wl,-rpath-link,${BINBOT_RKNN_LIBRARY_DIR}"
    )
    add_executable(binbot_face_embedding_probe
        "${PROJECT_SOURCE_DIR}/tools/face_embedding_probe.c"
    )
    target_link_libraries(binbot_face_embedding_probe PRIVATE binbot_vision)
    target_compile_options(binbot_face_embedding_probe PRIVATE
        -Wall -Wextra -Wpedantic
    )
else()
    target_sources(binbot_vision PRIVATE src/face_recognizer_stub.c)
endif()

target_compile_options(binbot_vision PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(binbot_vision PUBLIC m)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_face_matcher tests/test_face_matcher.c)
    target_link_libraries(test_face_matcher PRIVATE binbot_vision)
    target_compile_options(test_face_matcher PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME face_matcher COMMAND test_face_matcher)

    add_executable(test_face_overlay tests/test_face_overlay.c)
    target_link_libraries(test_face_overlay PRIVATE binbot_vision)
    target_compile_options(test_face_overlay PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME face_overlay COMMAND test_face_overlay)

    add_executable(test_hand_gesture tests/test_hand_gesture.c)
    target_link_libraries(test_hand_gesture PRIVATE binbot_vision)
    target_compile_options(test_hand_gesture PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hand_gesture COMMAND test_hand_gesture)

    add_executable(test_hand_overlay tests/test_hand_overlay.c)
    target_link_libraries(test_hand_overlay PRIVATE binbot_vision)
    target_compile_options(test_hand_overlay PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hand_overlay COMMAND test_hand_overlay)

    add_executable(test_cup_detection tests/test_cup_detection.c)
    target_include_directories(test_cup_detection PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(test_cup_detection PRIVATE binbot_vision)
    target_compile_options(test_cup_detection PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME cup_detection COMMAND test_cup_detection)

    add_executable(binbot_hand_gesture_probe
        "${PROJECT_SOURCE_DIR}/tools/hand_gesture_probe.c"
    )
    target_link_libraries(binbot_hand_gesture_probe PRIVATE binbot_vision)
    target_compile_options(binbot_hand_gesture_probe PRIVATE
        -Wall -Wextra -Wpedantic
    )

    set(_ROCKIVA_TEST_SDK
        "${RV1106_SDK_ROOT}/media/iva/iva/librockiva/rockiva-rv1106-Linux"
    )
    if(EXISTS "${_ROCKIVA_TEST_SDK}/include/rockiva_det_api.h")
        add_library(fake_rockiva SHARED tests/fake_rockiva.c)
        target_include_directories(fake_rockiva PRIVATE "${_ROCKIVA_TEST_SDK}/include")
        target_compile_options(fake_rockiva PRIVATE -Wall -Wextra -Wpedantic)
        set_target_properties(fake_rockiva PROPERTIES OUTPUT_NAME rockiva_test)

        add_executable(test_face_detector
            tests/test_face_detector.c
            src/face_detector_rockiva.c
        )
        target_include_directories(test_face_detector PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${_ROCKIVA_TEST_SDK}/include"
        )
        target_link_libraries(test_face_detector PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(test_face_detector PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME face_detector
            COMMAND test_face_detector $<TARGET_FILE:fake_rockiva>
        )
    endif()
endif()
