cmake_minimum_required(VERSION 3.22.1)
project("audioengine_android" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ─── Oboe (Google's low-latency audio library for Android) ─────────
include(FetchContent)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG 1.8.0
)
FetchContent_MakeAvailable(oboe)

# ─── Opus codec ────────────────────────────────────────────────────
# Pre-built opus is available via Android NDK, or we fetch it
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)
FetchContent_MakeAvailable(opus)

# ─── Core audio engine library (shared C code) ────────────────────
set(CORE_DIR "${CMAKE_SOURCE_DIR}/../../../../../../core")
file(GLOB CORE_SOURCES "${CORE_DIR}/src/*.c")

# ─── Native bridge library ─────────────────────────────────────────
add_library(audioengine_android SHARED
    native-bridge.cpp
    oboe-audio-io.cpp
    ${CORE_SOURCES}
)

target_include_directories(audioengine_android PRIVATE
    ${CORE_DIR}/include
    ${opus_SOURCE_DIR}/include
)

target_link_libraries(audioengine_android
    oboe
    opus
    android
    log
)

# Ensure we use OPUS_APPLICATION_AUDIO, not VOIP
target_compile_definitions(audioengine_android PRIVATE
    AE_PLATFORM_ANDROID=1
)
