#pragma once
#ifdef USE_HALOW_RADIO

#include "RadioInterface.h"
#include "concurrency/OSThread.h"
#include <stddef.h>
#include <stdint.h>

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
 * Mesh discovery uses standard 802.11 mesh advertisement IEs. The AP path is
 * intentionally not used for this transport.
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
    volatile bool scanInProgress = false;
    volatile bool meshPeerSeen = false;
    volatile bool nodeInfoPingPending = false;
    bool wlanReady = false;
    bool meshEnabled = false;
    uint32_t lastScanMs = 0;
    uint32_t lastMeshInfoMs = 0;
    uint32_t lastNodeInfoPingMs = 0;
    uint32_t lastScanStatusLogMs = 0;
    int16_t bestMeshRssi = -32768;
    uint8_t bestMeshBssid[6] = {0};
    char bestMeshId[33] = {0};
    char meshId[33] = {0};
    char meshKey[65] = {0};
    char countryCode[3] = {0};

    void onFrameReceived(const uint8_t *payload, size_t payload_len, int8_t rssi);
    void startMeshInfoRequest();
    bool loadMeshProfile();

#ifdef USE_MM_IOT_ESP32
    bool applyChannelList();
    bool startMeshStation();

    uint8_t meshScanIes[2 + MMWLAN_SSID_MAXLEN] = {0};
    struct mmwlan_scan_req meshScanReq = MMWLAN_SCAN_REQ_INIT;

    void onMeshScanResult(const struct mmwlan_scan_result *result);
    void onMeshScanComplete(enum mmwlan_scan_state scan_state);

    // Trampoline registered with mmwlan_register_rx_cb. The callback hands us
    // the 802.3 header and payload separately.
    static void rxTrampoline(uint8_t *header, unsigned header_len, uint8_t *payload, unsigned payload_len, void *arg);
    static void linkStateTrampoline(enum mmwlan_link_state link_state, void *arg);
    static void scanRxTrampoline(const struct mmwlan_scan_result *result, void *arg);
    static void scanCompleteTrampoline(enum mmwlan_scan_state scan_state, void *arg);
#endif

    static constexpr uint32_t MESH_INFO_SCAN_INTERVAL_MS = 30000;
    static constexpr uint32_t SCAN_STATUS_LOG_INTERVAL_MS = 10000;
    static constexpr uint32_t NODEINFO_PING_INTERVAL_MS = 60000;
    static constexpr uint8_t WLAN_IE_ID_MESH_CONFIG = 113;
    static constexpr uint8_t WLAN_IE_ID_MESH_ID = 114;
    // Approximate bytes-per-millisecond at the configured channel width / MCS.
    // HaLow is 150 kbps to 32.5 Mbps depending on configuration — picking a
    // single value is fiction, but airtime accounting needs *something*, and
    // duty cycle isn't the constraint on HaLow that it is on LoRa.
    static constexpr uint32_t HALOW_NOMINAL_KBPS = 1000; // 1 Mbps, 2 MHz MCS3 ballpark
};

#endif // USE_HALOW_RADIO
