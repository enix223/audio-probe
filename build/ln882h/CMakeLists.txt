# Recorder component for the LN882H SDK project.
#
# Included by ../../project/recorder/CMakeLists.txt (copied into the SDK by
# scripts/setup.sh). It lists the recorder sources and include paths; the SDK
# project owns the SDK/iot-client sources and the final executable.

get_filename_component(RECORDER_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RECORDER_SRC ${RECORDER_DIR}/src)

set(RECORDER_CORE_SRC
  ${RECORDER_SRC}/core/types/cl_recorder_types.cc
  ${RECORDER_SRC}/core/audio/cl_audio_stats.cc
  ${RECORDER_SRC}/core/audio/cl_opus_codec.cc
  ${RECORDER_SRC}/core/audio/cl_recorder_segmenter.cc
  ${RECORDER_SRC}/core/ws/cl_tx_queue.cc
  ${RECORDER_SRC}/core/ws/cl_recorder_client.cc
  ${RECORDER_SRC}/core/control/cl_recorder_commands.cc
  ${RECORDER_SRC}/core/control/cl_recorder_messages.cc
  ${RECORDER_SRC}/core/control/cl_mqtt_control.cc
  ${RECORDER_SRC}/core/tone/cl_recorder_tone.cc
  ${RECORDER_SRC}/core/app/cl_recorder_app.cc
)

set(RECORDER_LN_SRC
  ${RECORDER_SRC}/platforms/ln882h/protocol/cl_gx8006_protocol.cc
  ${RECORDER_SRC}/platforms/ln882h/audio/cl_gx8006_audio_source.cc
  ${RECORDER_SRC}/platforms/ln882h/uart/cl_ln882h_gx_uart.cc
  ${RECORDER_SRC}/platforms/ln882h/bsp/cl_gx8006_fc_pin.c
  ${RECORDER_SRC}/platforms/ln882h/bsp/cl_arm_tls_stub.c
  ${RECORDER_SRC}/platforms/ln882h/ws/cl_hal_ws_ln882h.cc
  ${RECORDER_SRC}/platforms/ln882h/app/cl_ln882h_app.cc
)

set(RECORDER_INCLUDES
  ${RECORDER_DIR}/protocol
  ${RECORDER_SRC}/core/types
  ${RECORDER_SRC}/core/audio
  ${RECORDER_SRC}/core/ws
  ${RECORDER_SRC}/core/control
  ${RECORDER_SRC}/core/app
  ${RECORDER_SRC}/core/tone
  ${RECORDER_SRC}/platforms/ln882h/protocol
  ${RECORDER_SRC}/platforms/ln882h/audio
  ${RECORDER_SRC}/platforms/ln882h/uart
  ${RECORDER_SRC}/platforms/ln882h/bsp
  ${RECORDER_SRC}/platforms/ln882h/ws
  ${RECORDER_SRC}/platforms/ln882h/config
  ${RECORDER_SRC}/platforms/ln882h/app
)

set(RECORDER_CORE_SRC "${RECORDER_CORE_SRC}" PARENT_SCOPE)
set(RECORDER_LN_SRC "${RECORDER_LN_SRC}" PARENT_SCOPE)
set(RECORDER_INCLUDES "${RECORDER_INCLUDES}" PARENT_SCOPE)
set(RECORDER_DIR "${RECORDER_DIR}" PARENT_SCOPE)
