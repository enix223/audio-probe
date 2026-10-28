/**
 * @file main.c
 * @brief LN882H recorder entry point.
 *
 * Brings up the SDK subsystems (clock, NVDS/KV, Wi-Fi, lwIP) and starts the
 * recorder task. The recorder uses MQTT for control and WebSocket for audio;
 * there is no wake-word/KWS engine.
 */

#include "flash_partition_table.h"
#include "hal/hal_interrupt.h"
#include "ln_kv_api.h"
#include "ln_misc.h"
#include "ln_nvds.h"
#include "ln_utils.h"
#include "netif/ethernetif.h"
#include "osal/osal.h"
#include "proj_config.h"
#include "serial.h"
#include "utils/debug/CmBacktrace/cm_backtrace.h"
#include "utils/reboot_trace/reboot_trace.h"
#include "utils/runtime/runtime.h"
#include "utils/system_parameter.h"
#include "wifi.h"
#include "wifi_port.h"

#include "cl_ln882h_app.h"

extern Serial_t m_LogSerial;

static void log_init_on_configured_port(void) {
#if (CONFIG_IOT_LOG_SERIAL_PORT == 0)
  log_init();
#else
  serial_init(&m_LogSerial, (serial_port_id_t)CONFIG_IOT_LOG_SERIAL_PORT, CFG_UART_BAUDRATE_LOG,
              NULL);
#endif
}

int main(int argc, char* argv[]) {
  LN_UNUSED(argc);
  LN_UNUSED(argv);

  ln_chip_get_reboot_cause();

  SetSysClock();
  set_interrupt_priority();
  switch_global_interrupt(HAL_ENABLE);
  ln_runtime_measure_init();

  OS_DefineHeapRegions();

  log_init_on_configured_port();
  LOG(LOG_LVL_INFO, "------  LN882H Audio Probe Recorder  ------\r\n");

  cm_backtrace_init("recorder", "hw", "sw");

  if (NVDS_ERR_OK != ln_nvds_init(NVDS_SPACE_OFFSET)) {
    LOG(LOG_LVL_ERROR, "NVDS init failed!\r\n");
  }
  if (KV_ERR_NONE != ln_kv_port_init(KV_SPACE_OFFSET, (KV_SPACE_OFFSET + KV_SPACE_SIZE))) {
    LOG(LOG_LVL_ERROR, "KV init failed!\r\n");
  }

  sysparam_integrity_check_all();
  wifi_rf_calibration();
  wifi_init();
  lwip_tcpip_init();

  if (0 != recorder_start()) {
    LOG(LOG_LVL_ERROR, "recorder_start failed!\r\n");
    return -1;
  }

  OS_ThreadStartScheduler();
  return 0;
}
