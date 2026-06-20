#pragma once
#ifdef USE_HALOW_RADIO

#include "RadioInterface.h"
#include "concurrency/OSThread.h"

#ifdef USE_MM_IOT_ESP32
extern "C" {
#include "mmwlan.h"
}
#endif

/**
 * HaLow (802.11ah) transport. Derives directly from RadioInterface because
 * RadioLib has no MM6108 driver and the model doesn't fit — mmwlan is a
 * frame-level API, not a register-level SPI interface.
 *
 * Frames go out as 802.3 payloads (broadcast MAC, EtherType 0x88B5) carrying
 * the same RadioBuffer the LoRa path builds via beginSending(), so the
 * Meshtastic wire format is unchanged.
 *
 * This mode associates with a HaLow AP, but does not initialize IP. The AP is
 * only the 802.11ah distribution system for raw Meshtastic frames.
 */
class HaLowInterface : public RadioInterface, private concurrency::OSThread
{
  public:
    HaLowInterface();
    ~HaLowInterface() override;

    bool init() override;
    bool reconfigure() override;
    bool sleep() override;
    bool canSleep() override;

    ErrorCode send(meshtastic_MeshPacket *p) override;
    meshtastic_QueueStatus getQueueStatus() override;
    uint32_t getPacketTime(uint32_t totalPacketLen, bool received = false) override;

  protected:
    int32_t runOnce() override;

  private:
    volatile bool linkUp = false;

    void onFrameReceived(const uint8_t *payload, size_t payload_len, int8_t rssi);

#ifdef USE_MM_IOT_ESP32
    // Trampoline registered with mmwlan_register_rx_cb. The callback hands us
    // the 802.3 header and payload separately.
    static void rxTrampoline(uint8_t *header, unsigned header_len, uint8_t *payload, unsigned payload_len, void *arg);
    static void linkStateTrampoline(enum mmwlan_link_state link_state, void *arg);
#endif

    // Approximate bytes-per-millisecond at the configured channel width / MCS.
    // HaLow is 150 kbps to 32.5 Mbps depending on configuration — picking a
    // single value is fiction, but airtime accounting needs *something*, and
    // duty cycle isn't the constraint on HaLow that it is on LoRa.
    static constexpr uint32_t HALOW_NOMINAL_KBPS = 1000; // 1 Mbps, 2 MHz MCS3 ballpark
};

#endif // USE_HALOW_RADIO
