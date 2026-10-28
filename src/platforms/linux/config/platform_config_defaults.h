#pragma once

#define CONFIG_IOT_PLATFORM_LINUX

// ---------------------
// Basic configuration
// ---------------------

// Health check interval (ms); 0 disables the periodic health check
#ifndef CONFIG_IOT_HEALTH_CHECK_INTERVAL_MS
#define CONFIG_IOT_HEALTH_CHECK_INTERVAL_MS 10000
#endif

// IoT product ID (integer, used for BLE provisioning key derivation)
#ifndef CONFIG_IOT_PRODUCT_ID_INT
#define CONFIG_IOT_PRODUCT_ID_INT 0
#endif

// IoT device ID (used across all platforms: OneNET device name, Azure device ID, AWS thing name)
#ifndef CONFIG_IOT_DEVICE_ID
#define CONFIG_IOT_DEVICE_ID "your-device-id"
#endif

#ifndef CONFIG_IOT_DEVICE_SECRET
// IoT device secret
#define CONFIG_IOT_DEVICE_SECRET ""
#endif

// Thing model JSON string
#ifndef CONFIG_IOT_THING_MODEL_STRING
#define CONFIG_IOT_THING_MODEL_STRING ""
#endif

// Thing model backend API URL - fetched before every MQTT connect (empty = disabled)
#ifndef CONFIG_IOT_THING_MODEL_API_URL
#define CONFIG_IOT_THING_MODEL_API_URL ""
#endif

// Timezone offset from UTC (e.g. 8 for CST, -5 for EST)
#ifndef CONFIG_IOT_TIMEZONE
#define CONFIG_IOT_TIMEZONE 8
#endif

// @select CONFIG_IOT_PLATFORM
#if !defined(CONFIG_IOT_PLATFORM_ONENET) && !defined(CONFIG_IOT_PLATFORM_AZURE) && !defined(CONFIG_IOT_PLATFORM_AWS) && !defined(CONFIG_IOT_PLATFORM_ALIOT) && !defined(CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT)
#define CONFIG_IOT_PLATFORM_ONENET
#endif

// Use mbedtls for HMAC-SHA1
#define CONFIG_IOT_HMAC_USE_MBEDTLS

// NTP server
#ifndef CONFIG_IOT_NTP_SERVER
#define CONFIG_IOT_NTP_SERVER "ntp.aliyun.com"
#endif

// Log level (0=error, 1=warn, 2=info, 3=debug)
#ifndef CONFIG_IOT_LOG_LEVEL
#define CONFIG_IOT_LOG_LEVEL 3
#endif

// Enable BLE provisioning (1 = enabled, 0 = disabled)
#ifndef CONFIG_IOT_BLE_PROVISION_ENABLE
#define CONFIG_IOT_BLE_PROVISION_ENABLE 0
#endif

#if CONFIG_IOT_BLE_PROVISION_ENABLE
// BLE advertising name (unused on Linux but needed to compile ble_provisioner.cpp)
#ifndef CONFIG_IOT_ADVERTISING_NAME
#define CONFIG_IOT_ADVERTISING_NAME "IoTClient"
#endif
#endif  // CONFIG_IOT_BLE_PROVISION_ENABLE

// WiFi SSID (unused on Linux — WiFi is OS-managed; must be defined)
#ifndef CONFIG_IOT_NETWORK_SSID
#define CONFIG_IOT_NETWORK_SSID ""
#endif

// WiFi password (unused on Linux — WiFi is OS-managed; must be defined)
#ifndef CONFIG_IOT_NETWORK_PASSWORD
#define CONFIG_IOT_NETWORK_PASSWORD ""
#endif

// Business serial port device (e.g. /dev/ttyUSB0, /dev/pts/N for pty)
#ifndef CONFIG_IOT_BIZ_SERIAL_DEVICE
#define CONFIG_IOT_BIZ_SERIAL_DEVICE "/dev/ttyUSB0"
#endif

// User serial (UART) TX GPIO pin (unused on Linux, must be defined)
#ifndef CONFIG_IOT_USER_SERIAL_TX_PIN
#define CONFIG_IOT_USER_SERIAL_TX_PIN 0
#endif

// User serial (UART) RX GPIO pin (unused on Linux, must be defined)
#ifndef CONFIG_IOT_USER_SERIAL_RX_PIN
#define CONFIG_IOT_USER_SERIAL_RX_PIN 0
#endif

// User serial (UART) baud rate (unused on Linux, must be defined)
#ifndef CONFIG_IOT_USER_SERIAL_BAUD
#define CONFIG_IOT_USER_SERIAL_BAUD 9600
#endif

// Enable Paho MQTT C internal trace logging (1=enabled, 0=disabled)
#define CONFIG_IOT_PAHO_TRACE_ENABLE 0

// ---------------------
// OneNET configuration
// ---------------------

// OneJSON protocol mode: 0 = raw data, 1 = OneJSON (default)
#ifndef CONFIG_IOT_PLATFORM_ONENET_PROTOCOL_ONEJSON
#define CONFIG_IOT_PLATFORM_ONENET_PROTOCOL_ONEJSON 1
#endif

#ifndef CONFIG_IOT_PLATFORM_ONENET_ENDPOINT
// OneNET MQTT endpoint
#define CONFIG_IOT_PLATFORM_ONENET_ENDPOINT "mqttstls.heclouds.com"
#endif

#ifndef CONFIG_IOT_PLATFORM_ONENET_PORT
// OneNET MQTT port
#define CONFIG_IOT_PLATFORM_ONENET_PORT 8883
#endif

#ifndef CONFIG_IOT_PLATFORM_ONENET_PRODUCT_ID
// OneNET product ID
#define CONFIG_IOT_PLATFORM_ONENET_PRODUCT_ID ""
#endif

// Device-level auth: 0 = product-level (uses product secret), 1 = device-level (uses device secret)
#ifndef CONFIG_IOT_PLATFORM_ONENET_DEVICE_LEVEL_AUTH
#define CONFIG_IOT_PLATFORM_ONENET_DEVICE_LEVEL_AUTH 0
#endif

// OneNET MQTT CA certificate (default: OneNET's private CA)
#ifndef CONFIG_IOT_PLATFORM_ONENET_CA_CERT
#define CONFIG_IOT_PLATFORM_ONENET_CA_CERT \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDOzCCAiOgAwIBAgIJAPCCNfxANtVEMA0GCSqGSIb3DQEBCwUAMDQxCzAJBgNV\n" \
  "BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDEVMBMGA1UEAwwMT25lTkVUIE1RVFRTMB4X\n" \
  "DTE5MDUyOTAxMDkyOFoXDTQ5MDUyMTAxMDkyOFowNDELMAkGA1UEBhMCQ04xDjAM\n" \
  "BgNVBAoMBUNNSU9UMRUwEwYDVQQDDAxPbmVORVQgTVFUVFMwggEiMA0GCSqGSIb3\n" \
  "DQEBAQUAA4IBDwAwggEKAoIBAQC/VvJ6lGWfy9PKdXKBdzY83OERB35AJhu+9jkx\n" \
  "5d4SOtZScTe93Xw9TSVRKrFwu5muGgPusyAlbQnFlZoTJBZY/745MG6aeli6plpR\n" \
  "r93G6qVN5VLoXAkvqKslLZlj6wXy70/e0GC0oMFzqSP0AY74icANk8dUFB2Q8usS\n" \
  "UseRafNBcYfqACzF/Wa+Fu/upBGwtl7wDLYZdCm3KNjZZZstvVB5DWGnqNX9HkTl\n" \
  "U9NBMS/7yph3XYU3mJqUZxryb8pHLVHazarNRppx1aoNroi+5/t3Fx/gEa6a5PoP\n" \
  "ouH35DbykmzvVE67GUGpAfZZtEFE1e0E/6IB84PE00llvy3pAgMBAAGjUDBOMB0G\n" \
  "A1UdDgQWBBTTi/q1F2iabqlS7yEoX1rbOsz5GDAfBgNVHSMEGDAWgBTTi/q1F2ia\n" \
  "bqlS7yEoX1rbOsz5GDAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAL\n" \
  "aqJ2FgcKLBBHJ8VeNSuGV2cxVYH1JIaHnzL6SlE5q7MYVg+Ofbs2PRlTiWGMazC7\n" \
  "q5RKVj9zj0z/8i3ScWrWXFmyp85ZHfuo/DeK6HcbEXJEOfPDvyMPuhVBTzuBIRJb\n" \
  "41M27NdIVCdxP6562n6Vp0gbE8kN10q+ksw8YBoLFP0D1da7D5WnSV+nwEIP+F4a\n" \
  "3ZX80bNt6tRj9XY0gM68mI60WXrF/qYL+NUz+D3Lw9bgDSXxpSN8JGYBR85BxBvR\n" \
  "NNAhsJJ3yoAvbPUQ4m8J/CoVKKgcWymS1pvEHmF47pgzbbjm5bdthlIx+swdiGFa\n" \
  "WzdhzTYwVkxBaU+xf/2w\n" \
  "-----END CERTIFICATE-----"
#endif

// ----------------------------
// Self-hosted MQTT configuration
// ----------------------------

// Self-hosted MQTT broker endpoint
#ifndef CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_ENDPOINT
#define CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_ENDPOINT "localhost"
#endif

// Self-hosted MQTT broker port
#ifndef CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_PORT
#define CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_PORT 1883
#endif

// Self-hosted MQTT CA certificate (PEM, empty = no TLS)
#ifndef CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_CA_CERT
#define CONFIG_IOT_PLATFORM_SELF_HOSTED_MQTT_CA_CERT ""
#endif

// ---------------------
// Azure IoT Hub configuration
// ---------------------

#ifndef CONFIG_IOT_PLATFORM_AZURE_ENDPOINT
// Azure IoT Hub hostname
#define CONFIG_IOT_PLATFORM_AZURE_ENDPOINT ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AZURE_PORT
// Azure IoT Hub MQTT port
#define CONFIG_IOT_PLATFORM_AZURE_PORT 8883
#endif
#ifndef CONFIG_IOT_PLATFORM_AZURE_CLIENT_CERT
// Azure IoT Hub client certificate (PEM)
#define CONFIG_IOT_PLATFORM_AZURE_CLIENT_CERT ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AZURE_PRIVATE_KEY
// Azure IoT Hub client private key (PEM)
#define CONFIG_IOT_PLATFORM_AZURE_PRIVATE_KEY ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AZURE_CA_CERT
// Azure IoT Hub CA certificate (PEM)
#define CONFIG_IOT_PLATFORM_AZURE_CA_CERT ""
#endif

// ---------------------
// AWS IoT Core configuration
// ---------------------

#ifndef CONFIG_IOT_PLATFORM_AWS_ENDPOINT
// AWS IoT Core endpoint
#define CONFIG_IOT_PLATFORM_AWS_ENDPOINT ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AWS_PORT
// AWS IoT Core MQTT port
#define CONFIG_IOT_PLATFORM_AWS_PORT 8883
#endif
#ifndef CONFIG_IOT_PLATFORM_AWS_CLIENT_CERT
// AWS IoT Core client certificate (PEM)
#define CONFIG_IOT_PLATFORM_AWS_CLIENT_CERT ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AWS_PRIVATE_KEY
// AWS IoT Core client private key (PEM)
#define CONFIG_IOT_PLATFORM_AWS_PRIVATE_KEY ""
#endif

#ifndef CONFIG_IOT_PLATFORM_AWS_CA_CERT
// AWS IoT Core CA certificate (PEM)
#define CONFIG_IOT_PLATFORM_AWS_CA_CERT ""
#endif

// ---------------------
// Alibaba Cloud IoT configuration
// ---------------------

#ifndef CONFIG_IOT_PLATFORM_ALIYUN_PRODUCT_KEY
// Alibaba Cloud IoT product key
#define CONFIG_IOT_PLATFORM_ALIYUN_PRODUCT_KEY ""
#endif

#ifndef CONFIG_IOT_PLATFORM_ALIYUN_REGION
// Alibaba Cloud IoT region
#define CONFIG_IOT_PLATFORM_ALIYUN_REGION "cn-shanghai"
#endif

#ifndef CONFIG_IOT_PLATFORM_ALIYUN_PORT
// Alibaba Cloud IoT port
#define CONFIG_IOT_PLATFORM_ALIYUN_PORT 443
#endif

// GlobalSign Root CA (Alibaba Cloud IoT production)
#ifndef CONFIG_IOT_PLATFORM_ALIYUN_CA_CERT
#define CONFIG_IOT_PLATFORM_ALIYUN_CA_CERT \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG\n" \
  "A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv\n" \
  "b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw\n" \
  "MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i\n" \
  "YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT\n" \
  "aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ\n" \
  "jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp\n" \
  "xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp\n" \
  "1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG\n" \
  "snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ\n" \
  "U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8\n" \
  "9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E\n" \
  "BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B\n" \
  "AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz\n" \
  "yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE\n" \
  "38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP\n" \
  "AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad\n" \
  "DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME\n" \
  "HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==\n" \
  "-----END CERTIFICATE-----"
#endif

// ---------------------
// HTTPS configuration
// ---------------------

// Global CA certificate bundle for all HTTPS requests (OTA check/download,
// thing-model fetch). Concatenate several PEM certificates in one value.
// Empty = use the platform trust store (unchanged behavior).
#ifndef CONFIG_IOT_HTTPS_CA_CERT
#define CONFIG_IOT_HTTPS_CA_CERT ""
#endif

// ---------------------
// OTA configuration
// ---------------------

// Enable OTA (Over-The-Air) firmware updates (1 = enabled, 0 = disabled)
#ifndef CONFIG_IOT_OTA_ENABLE
#define CONFIG_IOT_OTA_ENABLE 1
#endif

// OTA ECDSA P-256 signing public key (hex string, 128 chars: Qx || Qy)
// Generate: openssl ec -in key.pem -pubout -outform DER | tail -c 64 | xxd -p -c 64
// Empty string = skip signature verification (dev/test mode)
#ifndef CONFIG_IOT_OTA_SIGNING_PUBLIC_KEY
#define CONFIG_IOT_OTA_SIGNING_PUBLIC_KEY ""
#endif

// OTA polling interval in seconds (0 = disabled)
#ifndef CONFIG_IOT_OTA_POLL_INTERVAL_SEC
#define CONFIG_IOT_OTA_POLL_INTERVAL_SEC 0
#endif

// OTA backend API base URL (e.g. http://192.168.1.100:3002/api/ota/check)
#ifndef CONFIG_IOT_OTA_API_URL
#define CONFIG_IOT_OTA_API_URL ""
#endif

// OTA overall timeout in seconds (0 = disabled, default: 5 minutes)
#ifndef CONFIG_IOT_OTA_TIMEOUT_SEC
#define CONFIG_IOT_OTA_TIMEOUT_SEC 300
#endif

// Device metadata upload interval in seconds (0 = disabled, default: 3600)
#ifndef CONFIG_IOT_METADATA_UPLOAD_INTERVAL_SEC
#define CONFIG_IOT_METADATA_UPLOAD_INTERVAL_SEC 3600
#endif

// ---------------------
// Thread configuration
// ---------------------

// ---- Main-thread mode flags (see docs/plan-iot-client-memory-optimization.md) ----
// Main-thread mode: run the service as a non-blocking Poll() step on the
// thread that calls AppInit() (via TaskScheduler). 0 = dedicated thread.
#ifndef CONFIG_IOT_EVENT_BUS_IN_MAIN_THREAD
// Run EventBus as Poll() step on main thread (0 = dedicated thread)
#define CONFIG_IOT_EVENT_BUS_IN_MAIN_THREAD 1
#endif
#ifndef CONFIG_IOT_SYS_SERVICE_IN_MAIN_THREAD
// Run SystemService as Poll() step on main thread (0 = dedicated thread)
#define CONFIG_IOT_SYS_SERVICE_IN_MAIN_THREAD 1
#endif
#ifndef CONFIG_IOT_NETWORK_IN_MAIN_THREAD
// Run Network as Poll() step on main thread (0 = dedicated thread)
#define CONFIG_IOT_NETWORK_IN_MAIN_THREAD 1
#endif
#ifndef CONFIG_IOT_MQTT_IN_MAIN_THREAD
// Run MQTT as Poll() step on main thread (0 = dedicated thread)
#define CONFIG_IOT_MQTT_IN_MAIN_THREAD 0
#endif
#ifndef CONFIG_IOT_CMD_HANDLER_IN_MAIN_THREAD
// Run CommandHandler as Poll() step on main thread (0 = dedicated thread)
#define CONFIG_IOT_CMD_HANDLER_IN_MAIN_THREAD 0
#endif

// ---- Thread Stack Sizes (bytes) ----
// Increase if thread crashes with stack overflow; decrease to save RAM.

#ifndef CONFIG_IOT_STACK_EVENT_LOOP
// EventBus dispatch thread stack (bytes)
#define CONFIG_IOT_STACK_EVENT_LOOP 8192
#endif
#ifndef CONFIG_IOT_STACK_MQTT_LOOP
// MQTT client + TLS handshake thread stack (bytes)
#define CONFIG_IOT_STACK_MQTT_LOOP 8192
#endif
#ifndef CONFIG_IOT_STACK_NET_RECONNECT
// WiFi reconnect thread stack (bytes)
#define CONFIG_IOT_STACK_NET_RECONNECT 2048
#endif
#ifndef CONFIG_IOT_STACK_NTP_SYNC
// NTP sync thread stack (bytes)
#define CONFIG_IOT_STACK_NTP_SYNC 2048
#endif
#ifndef CONFIG_IOT_STACK_CMD_HANDLER
// Serial command handler thread stack (bytes)
#define CONFIG_IOT_STACK_CMD_HANDLER 4096
#endif
#ifndef CONFIG_IOT_STACK_HEALTH_CHECK
// System health check thread stack (bytes)
#define CONFIG_IOT_STACK_HEALTH_CHECK 1024
#endif
#ifndef CONFIG_IOT_STACK_OTA_WORKER
// On-demand OTA poll/download/verify thread stack (bytes)
#define CONFIG_IOT_STACK_OTA_WORKER 16384
#endif
#ifndef CONFIG_IOT_STACK_PROVISIONER
// BLE provisioning thread stack (bytes)
#define CONFIG_IOT_STACK_PROVISIONER 8192
#endif
