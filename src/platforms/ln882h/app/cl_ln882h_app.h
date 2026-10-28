#pragma once

#ifdef CONFIG_IOT_PLATFORM_LN882H

#ifdef __cplusplus
extern "C" {
#endif

// Starts the recorder application as a FreeRTOS task. Called by main() after
// the SDK has brought up the clock, Wi-Fi, and lwIP.
int recorder_start(void);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_IOT_PLATFORM_LN882H
