#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf54_meshtastic, LOG_LEVEL_DBG);

extern void setup();
extern void loop();

extern "C" void _init(void) {}
extern "C" void _fini(void) {}

extern "C" int main(void)
{
    LOG_INF("Meshtastic nRF54L15 full app starting");
    setup();

    while (true) {
        loop();
        k_yield();
    }

    return 0;
}
