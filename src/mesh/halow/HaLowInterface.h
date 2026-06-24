#pragma once
#ifdef USE_HALOW_RADIO

#include "RadioInterface.h"
#include "concurrency/OSThread.h"
#include <stddef.h>
#include <stdint.h>
#include <map>

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
    bool requestLocalMeshScan() override;

  protected:
    int32_t runOnce() override;

  private:
    static constexpr uint8_t WLAN_IE_ID_MESH_CONFIG = 113;
    static constexpr uint8_t WLAN_IE_ID_MESH_ID = 114;
    static constexpr uint8_t WLAN_IE_ID_VENDOR_SPECIFIC = 221;
    static constexpr uint8_t MESHTASTIC_VENDOR_OUI[3] = {'m', 's', 'h'};
    static constexpr uint8_t MESHTASTIC_VENDOR_TYPE_NODEINFO = 1;
    static constexpr uint8_t MESHTASTIC_VENDOR_VERSION = 1;
    static constexpr size_t MAX_DISCOVERY_VENDOR_IES = 2;
    static constexpr size_t MAX_VENDOR_IE_PAYLOAD_LEN = 255;
    static constexpr size_t MAX_VENDOR_IE_TOTAL_LEN = 2 + MAX_VENDOR_IE_PAYLOAD_LEN;
    static constexpr size_t MESHTASTIC_VENDOR_HEADER_LEN = 7;
    static constexpr size_t MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN = MAX_VENDOR_IE_PAYLOAD_LEN - MESHTASTIC_VENDOR_HEADER_LEN;

    volatile bool linkUp = false;
    volatile bool scanInProgress = false;
    volatile bool meshPeerSeen = false;
    volatile bool nodeInfoPingPending = false;
    bool wlanReady = false;
    bool meshEnabled = false;
    uint32_t lastScanMs = 0;
    uint32_t lastMeshInfoMs = 0;
    uint32_t lastNodeInfoPingMs = 0;
    uint32_t lastMeshStatusLogMs = 0;
    uint32_t staEventCount = 0;
    uint32_t staScanCount = 0;
    uint32_t staScanResultCount = 0;
    uint32_t staMeshAdvSeenCount = 0;
    uint32_t staTargetIdHitCount = 0;
    uint32_t staAuthReqCount = 0;
    uint32_t staAssocReqCount = 0;
    uint32_t staCtrlPortOpenCount = 0;
    int16_t bestMeshRssi = -32768;
    uint8_t bestMeshBssid[6] = {0};
    char bestMeshId[33] = {0};
    uint8_t discoveryVendorIe[MAX_DISCOVERY_VENDOR_IES * MAX_VENDOR_IE_TOTAL_LEN] = {0};
    size_t discoveryVendorIeLen = 0;
    char meshId[33] = {0};
    char meshKey[65] = {0};
    char countryCode[3] = {0};

    void onFrameReceived(const uint8_t *payload, size_t payload_len, int8_t rssi, const uint8_t *srcMac = nullptr,
                        size_t srcMacLen = 0);
    void onDiscoveryVendorIes(const uint8_t *ies, size_t iesLen, int8_t rssi, const uint8_t *srcMac = nullptr,
                              size_t srcMacLen = 0);
    void buildDiscoveryVendorIe();
    bool startMeshInfoRequest();
    bool loadMeshProfile();

#ifdef USE_MM_IOT_ESP32
    bool applyChannelList();
    bool startMeshStation();
    bool findHalowMacForNode(NodeNum nodeNum, uint8_t outMac[6]) const;
    bool findHalowMacForNodeAlias(NodeNum nodeNum, uint8_t hwModel, uint8_t outMac[6]) const;
    bool deriveHalowMacFromNode(NodeNum nodeNum, uint8_t hwModel, uint8_t outMac[6]) const;
    bool findHalowMacForNextHop(uint8_t nextHop, uint8_t outMac[6], NodeNum *resolvedNode = nullptr) const;
    bool getLocalHalowMac(uint8_t outMac[6]) const;
    bool resolveUnicastMac(const meshtastic_MeshPacket *p, uint8_t outMac[6], NodeNum &resolvedNodeNum) const;
    void cachePeerMac(NodeNum nodeNum, const uint8_t *mac, uint8_t hwModel = 0);

    struct mmwlan_beacon_vendor_ie_filter beaconVendorIeFilter = {};
    struct PeerMacCacheEntry {
        uint8_t mac[6];
        uint32_t lastSeenMs = 0;
    };
    static uint16_t makeAliasKey(uint8_t hardwareId, uint8_t nodeIdLowByte);
    std::map<NodeNum, PeerMacCacheEntry> peerMacCache;
    std::map<uint16_t, PeerMacCacheEntry> peerAliasCache;
    uint8_t meshScanIes[2 + MMWLAN_SSID_MAXLEN + sizeof(discoveryVendorIe)] = {0};
    struct mmwlan_scan_req meshScanReq = MMWLAN_SCAN_REQ_INIT;

    void onMeshScanResult(const struct mmwlan_scan_result *result);
    void onMeshScanComplete(enum mmwlan_scan_state scan_state);
    void onStaEvent(const struct mmwlan_sta_event_cb_args *sta_event);

    // Trampoline registered with mmwlan_register_rx_cb. The callback hands us
    // the 802.3 header and payload separately.
    static void rxTrampoline(uint8_t *header, unsigned header_len, uint8_t *payload, unsigned payload_len, void *arg);
    static void linkStateTrampoline(enum mmwlan_link_state link_state, void *arg);
    static void scanRxTrampoline(const struct mmwlan_scan_result *result, void *arg);
    static void scanCompleteTrampoline(enum mmwlan_scan_state scan_state, void *arg);
    static void staEventTrampoline(const struct mmwlan_sta_event_cb_args *sta_event, void *arg);
    static void beaconVendorIeTrampoline(const uint8_t *ies, uint32_t ies_len, void *arg);
#endif

    static constexpr uint32_t MESH_STATUS_LOG_INTERVAL_MS = 10000;
    static constexpr uint32_t NODEINFO_PING_INTERVAL_MS = 60000;
    static constexpr uint16_t MESH_CONNECT_SCAN_BASE_S = 60;
    static constexpr uint16_t MESH_CONNECT_SCAN_LIMIT_S = 600;
    // Approximate bytes-per-millisecond at the configured channel width / MCS.
    // HaLow is 150 kbps to 32.5 Mbps depending on configuration — picking a
    // single value is fiction, but airtime accounting needs *something*, and
    // duty cycle isn't the constraint on HaLow that it is on LoRa.
    static constexpr uint32_t HALOW_NOMINAL_KBPS = 1000; // 1 Mbps, 2 MHz MCS3 ballpark
};

#endif // USE_HALOW_RADIO
