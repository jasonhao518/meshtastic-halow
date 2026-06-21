#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ZephyrBluetooth.h"

LOG_MODULE_REGISTER(nrf54_meshtastic, LOG_LEVEL_DBG);

extern void setup();
extern void loop();

extern "C" void _init(void) {}
extern "C" void _fini(void) {}

extern "C" int main(void)
{
    LOG_INF("Meshtastic nRF54L15 full app starting");
    LOG_INF("Starting BLE worker before Meshtastic setup");
    nrf54BluetoothStartAsync();
    LOG_INF("Entering Meshtastic setup");
    setup();
    LOG_INF("Meshtastic setup returned");
    nrf54BluetoothMarkAppReady();
    nrf54BluetoothSetEnabled(true);

    while (true) {
        loop();
        k_yield();
    }

    return 0;
}
