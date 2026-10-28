# LN882H + GX8006 recorder project.
#
# Copied into $LN882H_SDK/project/recorder by build/ln882h/setup.sh, which
# replaces the /recorder placeholder with this repository's absolute path.
#
# The recorder uses MQTT (embedded-iot-client AppInit/IoTCore/thing model) for
# control and WebSocket for audio. The GX8006 microphone is chip-owned Opus in
# the first milestone; no wake-word/KWS engine is compiled.

include(ProjModuleCollect.cmake)

if(NOT DEFINED RECORDER_DIR OR RECORDER_DIR STREQUAL "")
  message(FATAL_ERROR "RECORDER_DIR is not set; run build/ln882h/setup.sh")
endif()

set(IOT_CLIENT_DIR ${RECORDER_DIR}/lib/embedded-iot-client)
set(IOT_CLIENT_PLATFORM_DIR ${IOT_CLIENT_DIR}/src/platform/ln882h)
set(RECORDER_SRC ${RECORDER_DIR}/src)
set(RECORDER_LN_CONFIG ${CMAKE_BINARY_DIR}/recorder_config)

# ---------------------------------------------------------------------------
# Recorder source lists (from the repository's build/ln882h component file)
# ---------------------------------------------------------------------------
include(${RECORDER_DIR}/build/ln882h/CMakeLists.txt)

# ---------------------------------------------------------------------------
# Generated platform configuration (credentials stay out of the tree)
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY ${RECORDER_LN_CONFIG})
set(RECORDER_MQTT_ENDPOINT "127.0.0.1" CACHE STRING "MQTT broker")
set(RECORDER_MQTT_PORT "8883" CACHE STRING "MQTT broker port")
set(RECORDER_MQTT_PRODUCT_ID "recorder-lab" CACHE STRING "IoT product id")
set(RECORDER_MQTT_DEVICE_ID "recorder-ln882h" CACHE STRING "IoT device id")
set(RECORDER_MQTT_DEVICE_SECRET "" CACHE STRING "IoT device secret (empty for the anonymous lab broker)")
set(RECORDER_MQTT_CA_FILE "" CACHE FILEPATH "MQTT CA PEM file (empty = no CA)")
set(RECORDER_WS_URL "ws://192.168.1.100:8080/v1/recorder" CACHE STRING "recorder WebSocket URL")
set(RECORDER_WS_TOKEN "" CACHE STRING "WebSocket bearer token")
set(RECORDER_WS_CA_FILE "" CACHE FILEPATH "WSS CA PEM file")
set(RECORDER_WS_INSECURE "0" CACHE STRING "1 = develop-only WSS without verification")
set(RECORDER_WIFI_SSID "" CACHE STRING "Wi-Fi SSID")
set(RECORDER_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password")
set(RECORDER_THING_MODEL
    "[{\"i\":1,\"n\":\"recorderCmd\",\"t\":9,\"l\":1024},{\"i\":2,\"n\":\"recorderStatus\",\"t\":9,\"l\":1024}]"
    CACHE STRING "thing-model JSON")

function(recorder_load_pem path out)
  set(_pem "")
  if(EXISTS "${path}")
    file(READ "${path}" _pem)
    string(REPLACE "\\" "\\\\" _pem "${_pem}")
    string(REPLACE "\r" "" _pem "${_pem}")
    string(REPLACE "\n" "\\n" _pem "${_pem}")
    string(REPLACE "\"" "\\\"" _pem "${_pem}")
  endif()
  set(${out} "${_pem}" PARENT_SCOPE)
endfunction()

recorder_load_pem("${RECORDER_MQTT_CA_FILE}" RECORDER_MQTT_CA_PEM)
recorder_load_pem("${RECORDER_WS_CA_FILE}" RECORDER_WS_CA_PEM)

configure_file(
  ${RECORDER_SRC}/platforms/ln882h/config/platform_config.h.in
  ${RECORDER_LN_CONFIG}/platform_config.h
  @ONLY)

# ---------------------------------------------------------------------------
# mbedTLS / HTTPS / SNTP / partition manager (SDK-provided)
# ---------------------------------------------------------------------------
file(GLOB MBEDTLS_SRC
  ${LN_SDK_ROOT}/components/net/mbedtls/library/*.c
  ${LN_SDK_ROOT}/components/net/mbedtls/port_ln/library/*.c)

set(HTTPSCLIENT_SRC
  ${LN_SDK_ROOT}/components/net/httpsclient/src/httpsclient.c
  ${LN_SDK_ROOT}/components/net/httpsclient/src/ca_cert_def.c)

set(SNTP_SRC
  ${LN_SDK_ROOT}/components/net/lwip-2.1.3/src/apps/sntp/sntp.c)

set(PARTITION_MGR_SRC
  ${LN_SDK_ROOT}/components/fs/partition_mgr/flash_partition_mgr.c)

# ---------------------------------------------------------------------------
# embedded-iot-client core (required for MQTT control/thing model)
# ---------------------------------------------------------------------------
set(IOT_CORE_SRC
  ${IOT_CLIENT_DIR}/src/core/cfgstore/cl_config_store.cpp
  ${IOT_CLIENT_DIR}/src/core/command/cl_command_handler.cpp
  ${IOT_CLIENT_DIR}/src/core/crypto/cl_hmac_sha1_mbedtls.cpp
  ${IOT_CLIENT_DIR}/src/core/crypto/cl_sha256.cpp
  ${IOT_CLIENT_DIR}/src/core/crypto/cl_aes.cpp
  ${IOT_CLIENT_DIR}/src/core/base64/cl_base64.cpp
  ${IOT_CLIENT_DIR}/src/core/eventbus/cl_eventbus.cpp
  ${IOT_CLIENT_DIR}/src/core/frame/cl_linkjob_frame.cpp
  ${IOT_CLIENT_DIR}/src/core/iot/cl_device_token.cpp
  ${IOT_CLIENT_DIR}/src/core/iot/cl_iot_core.cpp
  ${IOT_CLIENT_DIR}/src/core/iot/cl_onenet_iot_core.cpp
  ${IOT_CLIENT_DIR}/src/core/ntp/cl_ntp.cpp
  ${IOT_CLIENT_DIR}/src/core/system/cl_system_service.cpp
  ${IOT_CLIENT_DIR}/src/core/net/cl_network.cpp
  ${IOT_CLIENT_DIR}/src/core/sched/cl_task_scheduler.cpp
  ${IOT_CLIENT_DIR}/src/core/utils/cl_logger.cpp
  ${IOT_CLIENT_DIR}/src/core/thingmodel/cl_thing_model.cpp
  ${IOT_CLIENT_DIR}/src/core/http/cl_http_client.cpp
  ${IOT_CLIENT_DIR}/src/core/ota/cl_mbedtls_ecdsa_verifier.cpp
  ${IOT_CLIENT_DIR}/src/core/app/cl_init.cpp)

set(IOT_PLATFORM_SRC
  ${IOT_CLIENT_PLATFORM_DIR}/bsp/serial_hw.c
  ${IOT_CLIENT_PLATFORM_DIR}/bsp/flash_bridge.c
  ${IOT_CLIENT_PLATFORM_DIR}/serial/serial_bridge.c
  ${IOT_CLIENT_PLATFORM_DIR}/ble/ln882h_ble.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/http/ln882h_http_client.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/mqtt/ln882h_mqtt_client.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/net/ln882h_network.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/ntp/ln882h_ntp.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/ota/ln882h_ota_pal.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/platform_factory_ln882h.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/serial/ln882h_system_serial.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/serial/ln882h_user_serial.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/storage/ln882h_storage.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/system/ln882h_system_service.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/threading/ln882h_mutex.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/threading/ln882h_semaphore.cpp
  ${IOT_CLIENT_PLATFORM_DIR}/threading/ln882h_thread.cpp)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
set(TARGET_ELF_NAME ${USER_PROJECT})
set(pro_executable_target ${TARGET_ELF_NAME}.elf)

set(PROJ_ALL_SRC
  ${RECORDER_SRC}/platforms/ln882h/app/main.c
  ${IOT_CLIENT_PLATFORM_DIR}/startup/startup_${CHIP_SERIAL}_gcc.c
  ${MODULE_SRC}
  ${MBEDTLS_SRC}
  ${HTTPSCLIENT_SRC}
  ${SNTP_SRC}
  ${PARTITION_MGR_SRC}
  ${IOT_CORE_SRC}
  ${IOT_PLATFORM_SRC}
  ${RECORDER_CORE_SRC}
  ${RECORDER_LN_SRC})

set(COMP_DHCPD_SUPPORT ON PARENT_SCOPE)
set(COMP_LWIP_SUPPORT ON PARENT_SCOPE)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
set(IOT_CLIENT_INCLUDES
  ${IOT_CLIENT_DIR}/src
  ${IOT_CLIENT_DIR}/src/core
  ${IOT_CLIENT_PLATFORM_DIR}
  ${IOT_CLIENT_PLATFORM_DIR}/config
  ${IOT_CLIENT_PLATFORM_DIR}/bsp
  ${IOT_CLIENT_DIR}
  ${IOT_CLIENT_DIR}/lib/nonstd/any-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/optional-lite/include
  ${IOT_CLIENT_DIR}/lib/nonstd/variant-lite/include
  ${IOT_CLIENT_DIR}/lib/rapidjson/include)

# Inject proj_config.h for lwIP and other SDK modules.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I${IOT_CLIENT_PLATFORM_DIR}/config" CACHE INTERNAL "" FORCE)
add_compile_definitions(MBEDTLS_CONFIG_FILE="mbedtls_config.h")
add_compile_definitions(RAPIDJSON_ALLOCATOR_DEFAULT_CHUNK_CAPACITY=1024)
add_compile_definitions(RECORDER_MCU_LN882H=1)

include_directories(
  ${IOT_CLIENT_PLATFORM_DIR}/config
  ${CMAKE_CURRENT_SOURCE_DIR}/cfg
  ${RECORDER_LN_CONFIG}
  ${LN_SDK_ROOT}/components/net/mbedtls/include
  ${LN_SDK_ROOT}/components/net/mbedtls/port_ln/include
  ${LN_SDK_ROOT}/components/net/httpsclient/inc
  ${LN_SDK_ROOT}/components/aws/aws_port/logging-stack
  ${LN_SDK_ROOT}/components/aws/aws-iot-device-sdk-embedded-C/libraries/standard/coreMQTT/source/interface
  ${LN_SDK_ROOT}/components/fs/partition_mgr
  ${LN_SDK_ROOT}/mcu/driver_${CHIP_SERIAL})

# Keep rapidjson's per-document chunk small: LN882H has ~295 KB RAM and both
# MQTT and WS TLS can be resident while recording.
add_compile_options(-include${RECORDER_LN_CONFIG}/platform_config.h)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-frtti>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-include${IOT_CLIENT_PLATFORM_DIR}/variant_exception_fix.h>)

# ---------------------------------------------------------------------------
# Build info
# ---------------------------------------------------------------------------
add_custom_target(generate_build_info ALL
  COMMAND ${CMAKE_COMMAND}
    -DGIT_CMD=git
    -DIOT_CLIENT_DIR=${IOT_CLIENT_DIR}
    -P ${IOT_CLIENT_DIR}/build/generate_build_info.cmake
  BYPRODUCTS "${IOT_CLIENT_DIR}/src/core/cl_build_info.h")

add_executable(${pro_executable_target} ${PROJ_ALL_SRC})
add_dependencies(${pro_executable_target} generate_build_info)

target_link_libraries(${pro_executable_target}
  PUBLIC
  ${CHIP_SERIAL}_ble_full_stack
  ln::dhcpd
  lwip
  ${CHIP_SERIAL}_wifi
  -lc -lm -lnosys
  PRIVATE
  -T${LINKER_SCRIPT}
  ${EXTRA_LINK_FLAGS})

target_link_directories(${pro_executable_target} PRIVATE ${LN_SDK_ROOT}/lib/gcclib)

target_include_directories(${pro_executable_target}
  PRIVATE
  ${IOT_CLIENT_PLATFORM_DIR}/app
  ${IOT_CLIENT_PLATFORM_DIR}/bsp
  ${IOT_CLIENT_INCLUDES}
  ${RECORDER_INCLUDES})

include(gcc/gcc-custom-build-stage.cmake)
