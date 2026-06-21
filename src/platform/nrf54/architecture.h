#pragma once

#define ARCH_NRF54 1

#ifndef HAS_BLUETOOTH
#define HAS_BLUETOOTH 0
#endif
#ifndef HAS_SCREEN
#define HAS_SCREEN 0
#endif
#ifndef HAS_WIRE
#define HAS_WIRE 0
#endif
#ifndef HAS_GPS
#define HAS_GPS 0
#endif
#ifndef HAS_BUTTON
#define HAS_BUTTON 0
#endif
#ifndef HAS_TELEMETRY
#define HAS_TELEMETRY 0
#endif
#ifndef HAS_SENSOR
#define HAS_SENSOR 0
#endif
#ifndef HAS_RADIO
#define HAS_RADIO 1
#endif
#ifndef HAS_CUSTOM_CRYPTO_ENGINE
#define HAS_CUSTOM_CRYPTO_ENGINE 0
#endif

#define HW_VENDOR meshtastic_HardwareModel_PRIVATE_HW

#define LED_STATE_ON 1
#define LED_STATE_OFF 0

#define SEGGER_STDOUT_CH 0
