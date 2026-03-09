cmake_minimum_required(VERSION 3.20)
project(AudioPlatformWindows LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# ─── Find dependencies ────────────────────────────────────────
find_package(PkgConfig QUIET)

# Opus
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS opus)
endif()
if(NOT OPUS_FOUND)
    # Fetch Opus if not found
    include(FetchContent)
    FetchContent_Declare(opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.5.2
    )
    FetchContent_MakeAvailable(opus)
    set(OPUS_LIBRARIES opus)
endif()

# ─── Core audio engine library ─────────────────────────────────
set(CORE_DIR "${CMAKE_SOURCE_DIR}/../core")
file(GLOB CORE_SOURCES "${CORE_DIR}/src/*.c")

# ─── Common settings ──────────────────────────────────────────
set(COMMON_INCLUDES
    ${CORE_DIR}/include
    ${CORE_DIR}/src
    ${OPUS_INCLUDE_DIRS}
)

set(COMMON_LIBS
    ${OPUS_LIBRARIES}
    ws2_32       # Winsock
    ole32        # COM (for WASAPI)
    uuid         # COM GUIDs
    comctl32     # Common controls (sliders, etc.)
)

set(COMMON_DEFINES
    AE_PLATFORM_WINDOWS=1
    _WIN32_WINNT=0x0A00
    NOMINMAX
    AE_BUILDING_DLL
)

# ─── GUI client (main build target) ──────────────────────────
add_executable(audio_client WIN32
    src/main_gui.cpp
    src/wasapi_audio.cpp
    src/ws_signaling.cpp
    ${CORE_SOURCES}
)

target_include_directories(audio_client PRIVATE ${COMMON_INCLUDES})
target_link_libraries(audio_client ${COMMON_LIBS})
target_compile_definitions(audio_client PRIVATE ${COMMON_DEFINES})

# ─── Console client (for debugging) ──────────────────────────
add_executable(audio_client_cli
    src/main.cpp
    src/wasapi_audio.cpp
    ${CORE_SOURCES}
)

target_include_directories(audio_client_cli PRIVATE ${COMMON_INCLUDES})
target_link_libraries(audio_client_cli ${COMMON_LIBS})
target_compile_definitions(audio_client_cli PRIVATE ${COMMON_DEFINES})
