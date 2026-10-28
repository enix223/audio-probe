# Linux host build. Mirrors ai-voice's build/nix layout: the root CMakeLists
# delegates here, embedded-iot-client is consumed from lib/embedded-iot-client,
# and all output lands in out/linux.

set(RECORDER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(RECORDER_SRC ${RECORDER_ROOT}/src)
set(IOT_CLIENT_DIR ${RECORDER_ROOT}/lib/embedded-iot-client)

if(NOT EXISTS ${IOT_CLIENT_DIR}/CMakeLists.txt)
  message(FATAL_ERROR
    "lib/embedded-iot-client is missing. Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# Generated platform configuration (credentials stay out of the tree)
# ---------------------------------------------------------------------------
set(PLATFORM_CONFIG_DIR ${CMAKE_BINARY_DIR}/generated/platform)
file(MAKE_DIRECTORY ${PLATFORM_CONFIG_DIR})

set(RECORDER_MQTT_ENDPOINT "127.0.0.1" CACHE STRING "MQTT broker host")
set(RECORDER_MQTT_PORT "1883" CACHE STRING "MQTT broker port")
set(RECORDER_MQTT_PRODUCT_ID "recorder-lab" CACHE STRING "IoT product id")
set(RECORDER_MQTT_DEVICE_ID "recorder-linux" CACHE STRING "IoT device id")
set(RECORDER_MQTT_DEVICE_SECRET "" CACHE STRING "IoT device secret (empty for the anonymous lab broker)")
set(RECORDER_MQTT_CA_CERT "" CACHE STRING "MQTT CA bundle (empty for plaintext lab broker)")
set(RECORDER_LOG_LEVEL "3" CACHE STRING "0=error 1=warn 2=info 3=debug")
set(RECORDER_THING_MODEL
    "[{\"i\":1,\"n\":\"recorderCmd\",\"t\":9,\"l\":1024},{\"i\":2,\"n\":\"recorderStatus\",\"t\":9,\"l\":1024}]"
    CACHE STRING "Thing-model JSON string")

configure_file(
  ${RECORDER_SRC}/platforms/linux/config/platform_config.h.in
  ${PLATFORM_CONFIG_DIR}/platform_config.h
  @ONLY)
configure_file(
  ${RECORDER_SRC}/platforms/linux/config/platform_config_defaults.h
  ${PLATFORM_CONFIG_DIR}/platform_config_defaults.h
  COPYONLY)

# ---------------------------------------------------------------------------
# embedded-iot-client (submodule) — core + Linux platform subset
# ---------------------------------------------------------------------------
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory(${IOT_CLIENT_DIR} ${CMAKE_BINARY_DIR}/iotclient EXCLUDE_FROM_ALL)

file(GLOB_RECURSE IOT_LINUX_SOURCES ${IOT_CLIENT_DIR}/src/platform/linux/*.cpp)
list(REMOVE_ITEM IOT_LINUX_SOURCES ${IOT_CLIENT_DIR}/src/platform/linux/app/main.cpp)

add_library(recorder-iot-platform OBJECT ${IOT_LINUX_SOURCES})
target_include_directories(recorder-iot-platform PRIVATE
  ${IOT_CLIENT_DIR}/src/core
  ${IOT_CLIENT_DIR}/src/platform/linux
  ${IOT_CLIENT_DIR}/lib/cpp-httplib
  ${IOT_CLIENT_DIR}/lib/nonstd/any-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/optional-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/variant-lite/include
  ${IOT_CLIENT_DIR}/lib/rapidjson/include
  ${PLATFORM_CONFIG_DIR}
)
target_compile_definitions(recorder-iot-platform PRIVATE CPPHTTPLIB_MBEDTLS_SUPPORT)
target_compile_options(recorder-iot-platform PRIVATE -include platform_config.h)
target_link_libraries(recorder-iot-platform PUBLIC core paho-mqtt3as-static Threads::Threads)

# ---------------------------------------------------------------------------
# External dependencies (pinned tags, same versions as ai-voice)
# ---------------------------------------------------------------------------
include(FetchContent)
find_package(Threads REQUIRED)

FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.21
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniaudio)
add_library(recorder-miniaudio INTERFACE)
target_include_directories(recorder-miniaudio INTERFACE ${miniaudio_SOURCE_DIR})
target_compile_definitions(recorder-miniaudio INTERFACE
  MA_NO_DECODING MA_NO_ENCODING MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)

set(OPUS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  opus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG v1.5.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)
set(LWS_WITH_MBEDTLS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_OPENSSL ON CACHE BOOL "" FORCE)
set(LWS_UNIX_SOCK ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  libwebsockets
  GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
  GIT_TAG v4.3.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libwebsockets)

# ---------------------------------------------------------------------------
# recorder-core — platform-independent
# ---------------------------------------------------------------------------
set(RECORDER_CORE_SOURCES
  ${RECORDER_SRC}/core/types/cl_recorder_types.cc
  ${RECORDER_SRC}/core/audio/cl_audio_stats.cc
  ${RECORDER_SRC}/core/audio/cl_opus_codec.cc
  ${RECORDER_SRC}/core/audio/cl_recorder_segmenter.cc
  ${RECORDER_SRC}/core/ws/cl_tx_queue.cc
  ${RECORDER_SRC}/core/ws/cl_recorder_client.cc
  ${RECORDER_SRC}/core/control/cl_recorder_commands.cc
  ${RECORDER_SRC}/core/control/cl_recorder_messages.cc
  ${RECORDER_SRC}/core/tone/cl_recorder_tone.cc
  ${RECORDER_SRC}/core/app/cl_recorder_app.cc
)

add_library(recorder-core STATIC ${RECORDER_CORE_SOURCES})
target_include_directories(recorder-core PUBLIC
  ${RECORDER_ROOT}/protocol
  ${RECORDER_SRC}/core/types
  ${RECORDER_SRC}/core/audio
  ${RECORDER_SRC}/core/ws
  ${RECORDER_SRC}/core/control
  ${RECORDER_SRC}/core/app
  ${RECORDER_SRC}/core/tone
  ${IOT_CLIENT_DIR}/src/core
  ${IOT_CLIENT_DIR}/lib/nonstd/any-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/optional-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/variant-lite/include
  ${IOT_CLIENT_DIR}/lib/rapidjson/include
  ${PLATFORM_CONFIG_DIR}
)
target_compile_options(recorder-core PUBLIC -include platform_config.h)
target_link_libraries(recorder-core PUBLIC opus Threads::Threads)

# ---------------------------------------------------------------------------
# recorder-platform-linux — audio devices, WS transport, MQTT control
# ---------------------------------------------------------------------------
add_library(recorder-platform-linux STATIC
  ${RECORDER_SRC}/core/control/cl_mqtt_control.cc
  ${RECORDER_SRC}/platforms/linux/audio/cl_linux_audio_source.cc
  ${RECORDER_SRC}/platforms/linux/audio/cl_linux_audio_sink.cc
  ${RECORDER_SRC}/platforms/linux/audio/cl_file_audio_source.cc
  ${RECORDER_SRC}/platforms/linux/audio/cl_miniaudio_impl.c
  ${RECORDER_SRC}/platforms/linux/ws/cl_hal_ws_linux.cc
)
target_include_directories(recorder-platform-linux PUBLIC
  ${IOT_CLIENT_DIR}/src/platform/linux
  ${RECORDER_SRC}/platforms/linux/audio
  ${RECORDER_SRC}/platforms/linux/ws
)
target_link_libraries(recorder-platform-linux PUBLIC
  recorder-core
  recorder-miniaudio
  websockets
  Threads::Threads
)

# ---------------------------------------------------------------------------
# Executable and tests
# ---------------------------------------------------------------------------
# The iot platform objects are linked into the executable directly:
# CreatePlatformFactory lives there, while cl_init.cpp (in iot client's core)
# references it. Including the objects before the static libraries avoids the
# static link-order trap.
add_executable(recorder ${RECORDER_SRC}/platforms/linux/app/main.cc)
target_link_libraries(recorder PRIVATE recorder-platform-linux recorder-iot-platform)
set_target_properties(recorder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RECORDER_ROOT}/out/linux)

option(RECORDER_SANITIZE "Build with ASAN/UBSAN" OFF)
if(RECORDER_SANITIZE)
  foreach(t recorder-core recorder-platform-linux recorder)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

file(GLOB RECORDER_TEST_SOURCES ${RECORDER_ROOT}/tests/*.cc)
if(RECORDER_TEST_SOURCES)
  add_executable(recorder-tests
    ${RECORDER_TEST_SOURCES}
    ${RECORDER_SRC}/platforms/ln882h/protocol/cl_gx8006_protocol.cc
    ${RECORDER_SRC}/platforms/ln882h/audio/cl_gx8006_audio_source.cc)
  # core supplies the embedded-iot-client Logger implementation used by the
  # app's LOG_* macros. It is not otherwise started by the unit tests.
  target_link_libraries(recorder-tests PRIVATE recorder-core core)
  target_include_directories(recorder-tests PRIVATE
    ${RECORDER_ROOT}/tests
    ${RECORDER_SRC}/platforms/ln882h/protocol
    ${RECORDER_SRC}/platforms/ln882h/audio
    ${RECORDER_SRC}/platforms/ln882h/config)
  target_compile_definitions(recorder-tests PRIVATE RECORDER_ROOT="${RECORDER_ROOT}")
  set_target_properties(recorder-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${RECORDER_ROOT}/out/linux)
  enable_testing()
  add_test(NAME recorder-tests COMMAND recorder-tests)
endif()
