cmake_minimum_required(VERSION 3.14)
project(acestep-ggml LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# version.h: embed git commit hash into all binaries.
# runs on every build, only rewrites if the hash changed.
set(VERSION_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/version.h")
add_custom_target(version ALL
    COMMAND "${CMAKE_COMMAND}" "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT=${VERSION_OUTPUT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/version.cmake"
    BYPRODUCTS "${VERSION_OUTPUT}"
    COMMENT "Checking git version"
)

# pthread: required explicitly on older glibc (< 2.34) where libpthread
# is not merged into libc. Modern distros link it implicitly but aarch64
# and older x86_64 toolchains need the explicit dependency.
find_package(Threads REQUIRED)

# Suppress MSVC fopen/sprintf deprecation warnings
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Put executables and backend .so in the same directory (build root).
# Without this, ggml defaults to bin/ for .so but executables stay in root,
# and ggml_backend_load_all() can't find the backends at runtime.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# DiT tensor names can exceed default GGML_MAX_NAME of 64
add_compile_definitions(GGML_MAX_NAME=128)

# Harden: mark fread/fwrite/etc with warn_unused_result on all platforms
# SYCL excluded: _FORTIFY_SOURCE swaps memcpy for __memcpy_chk, unresolved in device code
if(NOT MSVC AND NOT GGML_SYCL)
    add_compile_definitions(_FORTIFY_SOURCE=2)
endif()

# CUDA architectures: cover Turing to Blackwell for distributed binaries.
# Users can override with -DCMAKE_CUDA_ARCHITECTURES=native for local builds.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND AND CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
        set(CMAKE_CUDA_ARCHITECTURES "75-virtual;80-virtual;86-real;89-real;120a-real;121a-real")
    else()
        set(CMAKE_CUDA_ARCHITECTURES "75-virtual;80-virtual;86-real;89-real")
    endif()
endif()

# ---------------------------------------------------------------------------
# Build flags. All default OFF so the out-of-the-box build is WAV-only with
# no external audio dependencies.
# ---------------------------------------------------------------------------
option(TOOLS "Build the six CLI tools (ace-synth, ace-lm, ace-understand, quantize, neural-codec, mp3-codec)." OFF)
option(FLAC "Enable FLAC encode and decode (libFLAC)" OFF)
option(OPUS "Enable Opus encode (libopusenc) and decode (libopusfile)" OFF)
option(MP3 "Enable MP3 encode (libmp3lame) and decode (vendored minimp3)" OFF)

# ---------------------------------------------------------------------------
# Optional codec detection. Each block fails configuration clearly when the
# requested library is missing.
# ---------------------------------------------------------------------------
if(MP3)
    find_library(MP3LAME_LIBRARY NAMES mp3lame)
    find_path(MP3LAME_INCLUDE_DIR lame/lame.h)
    if(NOT MP3LAME_LIBRARY OR NOT MP3LAME_INCLUDE_DIR)
        message(FATAL_ERROR
            "-DMP3=ON requires libmp3lame (LAME). On Debian/Ubuntu install libmp3lame-dev.")
    endif()
endif()

if(FLAC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLAC REQUIRED flac IMPORTED_TARGET)
endif()

if(OPUS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUSENC REQUIRED libopusenc IMPORTED_TARGET)
    pkg_check_modules(OPUSFILE REQUIRED opusfile IMPORTED_TARGET)
endif()

# ggml as subdirectory, inherits GGML_CUDA, GGML_METAL, etc. from cmake flags
add_subdirectory(ggml)

# cpp-httplib (HTTP server library, used by ace-server)
add_subdirectory(vendor/cpp-httplib)

# Shared compile options and ggml linkage
macro(link_ggml_backends target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}
    )
    target_include_directories(${target} SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/ggml/include
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4505)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion
                              -Wno-unused-parameter -Wno-unused-function -Wno-sign-conversion)
    endif()
    target_link_libraries(${target} PRIVATE ggml Threads::Threads)
    if(TARGET ggml-base)
        target_link_libraries(${target} PRIVATE ggml-base)
    endif()
    foreach(backend cpu blas cuda metal vulkan sycl)
        if(TARGET ggml-${backend})
            get_target_property(CURRENT_BACKEND_TYPE ggml-${backend} TYPE)
            if (CURRENT_BACKEND_TYPE STREQUAL "MODULE_LIBRARY")
                # DL mode: backend is loaded at runtime via dlopen,
                # skip all link-time deps.
                continue()
            endif()
            target_link_libraries(${target} PRIVATE ggml-${backend})
        endif()
    endforeach()

    # SYCL links its runtime PRIVATE, consumers need -fsycl to resolve libsycl.so
    if(TARGET ggml-sycl AND GGML_SYCL)
        target_link_options(${target} PRIVATE -fsycl)
    endif()
    add_dependencies(${target} version)
endmacro()

# yyjson (MIT, fast JSON parser/writer)
add_library(yyjson STATIC vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson)
if(MSVC)
    target_compile_options(yyjson PRIVATE /W0)
else()
    target_compile_options(yyjson PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# acestep-audio: compiled audio IO (WAV always, optional MP3/Opus/FLAC).
# No GGML dependency. ace-server and audio-using tools link this.
# ---------------------------------------------------------------------------
add_library(acestep-audio STATIC src/audio-io.cpp)
target_include_directories(acestep-audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(acestep-audio PUBLIC Threads::Threads)
if(NOT MSVC)
    target_link_libraries(acestep-audio PUBLIC m)
    target_compile_options(acestep-audio PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()
add_dependencies(acestep-audio version)

if(MP3)
    target_compile_definitions(acestep-audio PUBLIC ACE_HAVE_MP3)
    target_include_directories(acestep-audio PRIVATE ${MP3LAME_INCLUDE_DIR})
    target_link_libraries(acestep-audio PUBLIC ${MP3LAME_LIBRARY})
endif()

if(FLAC)
    target_compile_definitions(acestep-audio PUBLIC ACE_HAVE_FLAC)
    target_link_libraries(acestep-audio PUBLIC PkgConfig::FLAC)
endif()

if(OPUS)
    target_compile_definitions(acestep-audio PUBLIC ACE_HAVE_OPUS)
    target_link_libraries(acestep-audio PUBLIC PkgConfig::OPUSENC PkgConfig::OPUSFILE)
endif()

# Core library (shared between binaries)
add_library(acestep-core STATIC
    src/request.cpp
    src/model-store.cpp
    src/pipeline-lm.cpp
    src/pipeline-synth.cpp
    src/pipeline-synth-ops.cpp
    src/pipeline-understand.cpp
)
target_link_libraries(acestep-core PUBLIC yyjson)
link_ggml_backends(acestep-core)

# ---------------------------------------------------------------------------
# ace-server (always built): links acestep-core + acestep-audio + httplib.
# ---------------------------------------------------------------------------
# webui: convert tools/webui/public/index.html.gz to a C header for embedding.
# the .gz is committed to git so the C++ build works without npm.
# to update: cd tools/webui && pnpm install && pnpm run build, then rebuild ace-server.
#
# Two-stage: first substitute @DEFAULTFORMAT@ with the build-time default,
# then xxd the resulting .gz into a C header.
if(MP3)
    set(WEBUI_DEFAULT_FORMAT "mp3")
elseif(OPUS)
    set(WEBUI_DEFAULT_FORMAT "opus")
elseif(FLAC)
    set(WEBUI_DEFAULT_FORMAT "flac")
else()
    set(WEBUI_DEFAULT_FORMAT "wav16")
endif()

find_program(GZIP_EXECUTABLE gzip HINTS "C:/Program Files/Git/usr/bin" "C:/Program Files (x86)/Git/usr/bin")
find_program(SED_EXECUTABLE sed HINTS "C:/Program Files/Git/usr/bin" "C:/Program Files (x86)/Git/usr/bin")
if(NOT GZIP_EXECUTABLE)
    message(FATAL_ERROR "gzip not found. Install gzip to build the embedded WebUI.")
endif()
if(NOT SED_EXECUTABLE)
    message(FATAL_ERROR "sed not found. Install sed to build the embedded WebUI.")
endif()

set(WEBUI_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tools/public/index.html.gz")
set(WEBUI_PREPROCESSED_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
set(WEBUI_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz.hpp")

add_custom_command(
    OUTPUT "${WEBUI_PREPROCESSED_GZ}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${WEBUI_INPUT}"
        "-DOUTPUT=${WEBUI_PREPROCESSED_GZ}"
        "-DDEFAULT_FORMAT=${WEBUI_DEFAULT_FORMAT}"
        "-DGZIP_EXECUTABLE=${GZIP_EXECUTABLE}"
        "-DSED_EXECUTABLE=${SED_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/webui-substitute.cmake"
    DEPENDS "${WEBUI_INPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/webui-substitute.cmake"
    COMMENT "Substituting @DEFAULTFORMAT@ -> ${WEBUI_DEFAULT_FORMAT} in webui"
)

add_custom_command(
    OUTPUT "${WEBUI_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${WEBUI_PREPROCESSED_GZ}" "-DOUTPUT=${WEBUI_OUTPUT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/xxd.cmake"
    DEPENDS "${WEBUI_PREPROCESSED_GZ}"
    COMMENT "Embedding webui into index.html.gz.hpp"
)
set_source_files_properties(${WEBUI_OUTPUT} PROPERTIES GENERATED TRUE)

add_executable(ace-server tools/ace-server.cpp ${WEBUI_OUTPUT})
target_include_directories(ace-server PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(ace-server PRIVATE acestep-core acestep-audio httplib)
link_ggml_backends(ace-server)

# ---------------------------------------------------------------------------
# CLI tools (gated by -DTOOLS=ON)
# ---------------------------------------------------------------------------
if(TOOLS)
    # ace-synth: full pipeline (text-enc + cond + dit + vae + audio out)
    add_executable(ace-synth tools/ace-synth.cpp)
    target_link_libraries(ace-synth PRIVATE acestep-core acestep-audio)
    link_ggml_backends(ace-synth)

    # ace-lm: LLM inference (CoT + audio codes)
    add_executable(ace-lm tools/ace-lm.cpp)
    target_link_libraries(ace-lm PRIVATE acestep-core)
    link_ggml_backends(ace-lm)

    # ace-understand: reverse pipeline (audio -> metadata)
    add_executable(ace-understand tools/ace-understand.cpp)
    target_link_libraries(ace-understand PRIVATE acestep-core acestep-audio)
    link_ggml_backends(ace-understand)

    # quantize: GGUF requantizer (BF16 -> K-quants)
    add_executable(quantize tools/quantize.cpp)
    link_ggml_backends(quantize)

    # neural-codec: Oobleck VAE neural audio codec (encode/decode WAV <-> latent)
    add_executable(neural-codec tools/neural-codec.cpp)
    target_link_libraries(neural-codec PRIVATE acestep-audio)
    link_ggml_backends(neural-codec)

    # mp3-codec: standalone audio transcoder (.wav/.mp3/.opus/.flac).
    # Generic converter; no GGML dependency. The binary keeps the historical
    # name even though it now handles more than MP3.
    add_executable(mp3-codec tools/mp3-codec.cpp)
    target_link_libraries(mp3-codec PRIVATE acestep-audio)
    target_include_directories(mp3-codec PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_dependencies(mp3-codec version)
    if(MSVC)
        target_compile_options(mp3-codec PRIVATE /W4 /wd4100 /wd4505)
    else()
        target_compile_options(mp3-codec PRIVATE -Wall -Wextra -Wconversion
                              -Wno-unused-parameter -Wno-unused-function -Wno-sign-conversion)
    endif()
endif()

add_subdirectory(tests)
