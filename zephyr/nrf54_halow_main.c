#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf54_halow, LOG_LEVEL_DBG);

int main(void)
{
    LOG_INF("Meshtastic nRF54L15 HaLow bring-up starting");
    LOG_DBG("nRF54L15 HaLow debug logging enabled on SEGGER RTT/J-Link");

#if DT_NODE_EXISTS(DT_NODELABEL(morse_wifi))
    const struct device *morse = DEVICE_DT_GET(DT_NODELABEL(morse_wifi));
    LOG_INF("Morse node present, ready=%d", device_is_ready(morse));
#else
    LOG_WRN("Morse node missing from devicetree");
#endif

    while (true) {
        LOG_INF("nRF54L15 HaLow heartbeat");
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
