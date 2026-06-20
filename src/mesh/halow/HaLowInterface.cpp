#include "configuration.h"
#ifdef USE_HALOW_RADIO

#include "HaLowFrame.h"
#include "HaLowInterface.h"
#include "Channels.h"
#include "MeshRadio.h"
#include "MeshTypes.h"
#include "RTC.h" // getValidTime / RTCQualityFromNet
#include "Throttle.h"
#include "main.h"
#include <algorithm>
#include <string.h>

#ifdef USE_MM_IOT_ESP32
extern "C" {
#include "mmpkt.h"
#include "mmhal.h"
#include "mmregdb.h"
#include "mmwlan.h"
}

#ifndef HALOW_MESH_SCAN_DWELL_MS
#define HALOW_MESH_SCAN_DWELL_MS 120
#endif
#endif

static const char *regionToHaLowCountryCode(meshtastic_Config_LoRaConfig_RegionCode region)
{
    switch (region) {
    case meshtastic_Config_LoRaConfig_RegionCode_US:
        return "US";
    case meshtastic_Config_LoRaConfig_RegionCode_EU_433:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_868:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_866:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_874:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_917:
    case meshtastic_Config_LoRaConfig_RegionCode_EU_N_868:
    case meshtastic_Config_LoRaConfig_RegionCode_UA_433:
    case meshtastic_Config_LoRaConfig_RegionCode_UA_868:
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_433:
    case meshtastic_Config_LoRaConfig_RegionCode_KZ_863:
        return "EU";
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ:
    case meshtastic_Config_LoRaConfig_RegionCode_ANZ_433:
        return "AU";
    case meshtastic_Config_LoRaConfig_RegionCode_NZ_865:
        return "NZ";
    case meshtastic_Config_LoRaConfig_RegionCode_IN:
    case meshtastic_Config_LoRaConfig_RegionCode_NP_865:
        return "IN";
    case meshtastic_Config_LoRaConfig_RegionCode_JP:
        return "JP";
    case meshtastic_Config_LoRaConfig_RegionCode_KR:
        return "KR";
    case meshtastic_Config_LoRaConfig_RegionCode_SG_923:
        return "SG";
    default:
        return nullptr;
    }
}

static size_t expandPrimaryPsk(uint8_t *out, size_t outLen)
{
    const auto &primary = channels.getPrimary();
    if (primary.psk.size == 0) {
        return 0;
    }

    size_t pskLen = std::min((size_t)primary.psk.size, outLen);
    memcpy(out, primary.psk.bytes, pskLen);
    if (pskLen == 1) {
        uint8_t pskIndex = out[0];
        if (pskIndex == 0) {
            return 0;
        }
        memcpy(out, defaultpsk, sizeof(defaultpsk));
        out[sizeof(defaultpsk) - 1] = (uint8_t)(out[sizeof(defaultpsk) - 1] + pskIndex - 1);
        return sizeof(defaultpsk);
    }
    if (pskLen < 16) {
        memset(out + pskLen, 0, 16 - pskLen);
        return 16;
    }
    if (pskLen != 16 && pskLen < 32) {
        memset(out + pskLen, 0, 32 - pskLen);
        return 32;
    }
    return pskLen;
}

static void bytesToHex(const uint8_t *bytes, size_t len, char *out, size_t outLen)
{
    static constexpr char hex[] = "0123456789abcdef";
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < outLen; i++) {
        out[pos++] = hex[bytes[i] >> 4];
        out[pos++] = hex[bytes[i] & 0x0f];
    }
    out[pos] = '\0';
}

HaLowInterface::HaLowInterface() : concurrency::OSThread("HaLow") {}

#ifdef USE_MM_IOT_ESP32
void HaLowInterface::linkStateTrampoline(enum mmwlan_link_state link_state, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (!self) {
        return;
    }

    if (link_state == MMWLAN_LINK_UP) {
        self->linkUp = true;
        LOG_INFO("HaLow: link UP");
    } else {
        self->linkUp = false;
        LOG_INFO("HaLow: link DOWN");
    }
}

// mmwlan delivers Ethernet-framed packets here: 14-byte 802.3 header + payload.
// The payload is whatever EtherType we used on the TX side — we filter for our
// own marker and decode the inner RadioBuffer.
void HaLowInterface::rxTrampoline(uint8_t *header, unsigned header_len, uint8_t *payload, unsigned payload_len, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (!self || header_len < sizeof(HaLowEthFrameHeader)) {
        return;
    }
    // EtherType is big-endian in the header (bytes 12-13).
    uint16_t et = ((uint16_t)header[12] << 8) | header[13];
    if (et != ETHERTYPE_MESHTASTIC_HALOW) {
        return;
    }
    self->onFrameReceived(payload, payload_len, /*rssi*/ 0);
}

void HaLowInterface::scanRxTrampoline(const struct mmwlan_scan_result *result, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (self) {
        self->onMeshScanResult(result);
    }
}

void HaLowInterface::scanCompleteTrampoline(enum mmwlan_scan_state scan_state, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (self) {
        self->onMeshScanComplete(scan_state);
    }
}
#endif

HaLowInterface::~HaLowInterface() = default;

bool HaLowInterface::init()
{
    RadioInterface::init();

#ifdef USE_MM_IOT_ESP32
    if (!loadMeshProfile()) {
        return false;
    }

    LOG_INFO("HaLow: mmhal_init()");
    mmhal_init();

    LOG_INFO("HaLow: mmwlan_init()");
    mmwlan_init();

    const struct mmwlan_s1g_channel_list *channel_list = mmwlan_lookup_regulatory_domain(get_regulatory_db(), countryCode);
    if (!channel_list) {
        LOG_ERROR("HaLow: country %s not in regdb", countryCode);
        return false;
    }
    if (mmwlan_set_channel_list(channel_list) != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: set_channel_list failed");
        return false;
    }

    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    enum mmwlan_status st = mmwlan_boot(&boot_args);
    if (st != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: mmwlan_boot failed (%d) — firmware load or SPI wiring", (int)st);
        return false;
    }

    struct mmwlan_version version;
    if (mmwlan_get_version(&version) == MMWLAN_SUCCESS) {
        LOG_INFO("HaLow: chip 0x%lx, fw %s, lib %s", (unsigned long)version.morse_chip_id, version.morse_fw_version,
                 version.morselib_version);
    }

    mmwlan_register_link_state_cb(linkStateTrampoline, this);
    if (mmwlan_register_rx_cb(rxTrampoline, this) != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: register_rx_cb failed");
        return false;
    }

    struct mmwlan_scan_config scanConfig = MMWLAN_SCAN_CONFIG_INIT;
    scanConfig.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
    scanConfig.home_channel_dwell_time_ms = 0;
    if (mmwlan_set_scan_config(&scanConfig) != MMWLAN_SUCCESS) {
        LOG_WARN("HaLow: set mesh scan config failed");
    }

    struct mmwlan_sta_args staArgs = MMWLAN_STA_ARGS_INIT;
    staArgs.ssid_len = strnlen(meshId, sizeof(staArgs.ssid));
    memcpy(staArgs.ssid, meshId, staArgs.ssid_len);
    staArgs.passphrase_len = strnlen(meshKey, sizeof(meshKey));
    memcpy(staArgs.passphrase, meshKey, staArgs.passphrase_len);
    staArgs.security_type = staArgs.passphrase_len > 0 ? MMWLAN_SAE : MMWLAN_OPEN;
    staArgs.scan_rx_cb = scanRxTrampoline;
    staArgs.scan_rx_cb_arg = this;
    staArgs.scan_interval_base_s = 1;
    staArgs.scan_interval_limit_s = 8;
    staArgs.mesh_mode = true;

    enum mmwlan_status meshStatus = mmwlan_sta_enable(&staArgs, NULL);
    if (meshStatus != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: mesh STA enable failed (%d)", (int)meshStatus);
        return false;
    }

    LOG_INFO("HaLow: 802.11s mesh enabled id='%s' country=%s key=%s", meshId, countryCode,
             staArgs.passphrase_len > 0 ? "primary-psk" : "open");
    startMeshInfoRequest();
    return true;
#else
    LOG_WARN("HaLow: built without USE_MM_IOT_ESP32, transport is a stub");
    return false;
#endif
}

bool HaLowInterface::reconfigure()
{
    return true;
}

bool HaLowInterface::sleep()
{
    return true;
}

bool HaLowInterface::canSleep()
{
    return true;
}

bool HaLowInterface::loadMeshProfile()
{
    const char *country = regionToHaLowCountryCode(config.lora.region);
    if (!country) {
        LOG_ERROR("HaLow: Meshtastic region %d has no Morse S1G country mapping", (int)config.lora.region);
        return false;
    }
    strncpy(countryCode, country, sizeof(countryCode));
    countryCode[sizeof(countryCode) - 1] = '\0';

    const char *primaryName = channels.getName(channels.getPrimaryIndex());
    if (!primaryName || primaryName[0] == '\0') {
        primaryName = "LongFast";
    }
    strncpy(meshId, primaryName, sizeof(meshId));
    meshId[sizeof(meshId) - 1] = '\0';

    uint8_t psk[32] = {0};
    size_t pskLen = expandPrimaryPsk(psk, sizeof(psk));
    if (pskLen == 0) {
        meshKey[0] = '\0';
    } else {
        bytesToHex(psk, pskLen, meshKey, sizeof(meshKey));
    }

    LOG_INFO("HaLow: profile from Meshtastic country=%s mesh_id='%s' key=%s", countryCode, meshId,
             meshKey[0] ? "primary-channel" : "open");
    return true;
}

ErrorCode HaLowInterface::send(meshtastic_MeshPacket *p)
{
    if (!p) {
        return ERRNO_UNKNOWN;
    }

#ifdef USE_MM_IOT_ESP32
    if (!linkUp) {
        packetPool.release(p);
        return ERRNO_DISABLED;
    }

    // beginSending() serializes the MeshPacket into radioBuffer (PacketHeader
    // + payload). We then prepend a 14-byte 802.3 header for mmwlan.
    size_t encoded = beginSending(p);
    if (encoded == 0) {
        packetPool.release(p);
        return ERRNO_UNKNOWN;
    }

    uint8_t txbuf[sizeof(HaLowEthFrameHeader) + sizeof(RadioBuffer)];
    if (encoded > sizeof(RadioBuffer)) {
        LOG_ERROR("HaLow: encoded %u > radioBuffer", (unsigned)encoded);
        packetPool.release(p);
        return ERRNO_UNKNOWN;
    }

    // Ethernet header: DA(6) || SA(6) || EtherType(2, big-endian).
    memcpy(txbuf, HALOW_BROADCAST_MAC, 6);
    if (mmwlan_get_mac_addr(txbuf + 6) != MMWLAN_SUCCESS) {
        memset(txbuf + 6, 0, 6); // fallback so the frame still goes out
    }
    txbuf[12] = (uint8_t)(ETHERTYPE_MESHTASTIC_HALOW >> 8);
    txbuf[13] = (uint8_t)(ETHERTYPE_MESHTASTIC_HALOW & 0xFF);
    memcpy(txbuf + sizeof(HaLowEthFrameHeader), &radioBuffer, encoded);

    enum mmwlan_status st = mmwlan_tx_wait_until_ready(0);
    if (st == MMWLAN_SUCCESS) {
        struct mmwlan_tx_metadata metadata = MMWLAN_TX_METADATA_INIT;
        struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(sizeof(HaLowEthFrameHeader) + encoded, metadata.tid);
        if (pkt) {
            struct mmpktview *pktview = mmpkt_open(pkt);
            mmpkt_append_data(pktview, txbuf, sizeof(HaLowEthFrameHeader) + encoded);
            mmpkt_close(&pktview);
            st = mmwlan_tx_pkt(pkt, &metadata);
        } else {
            st = MMWLAN_NO_MEM;
        }
    }

    packetPool.release(p);
    sendingPacket = NULL;
    return (st == MMWLAN_SUCCESS) ? ERRNO_OK : ERRNO_UNKNOWN;
#else
    packetPool.release(p);
    return ERRNO_DISABLED;
#endif
}

meshtastic_QueueStatus HaLowInterface::getQueueStatus()
{
    meshtastic_QueueStatus qs = meshtastic_QueueStatus_init_zero;
    qs.free = linkUp ? 16 : 0;
    qs.maxlen = 16;
    return qs;
}

uint32_t HaLowInterface::getPacketTime(uint32_t totalPacketLen, bool /*received*/)
{
    // bytes * 8 bits / (HALOW_NOMINAL_KBPS * 1000 bits/sec) * 1000 ms/sec.
    // Floor to 1 ms so the slot-time math upstream never divides by zero.
    uint32_t ms = (totalPacketLen * 8u + HALOW_NOMINAL_KBPS - 1u) / HALOW_NOMINAL_KBPS;
    return ms ? ms : 1u;
}

int32_t HaLowInterface::runOnce()
{
#ifdef USE_MM_IOT_ESP32
    if (!scanInProgress && !Throttle::isWithinTimespanMs(lastScanMs, MESH_INFO_SCAN_INTERVAL_MS)) {
        startMeshInfoRequest();
    }
#endif
    return 1000;
}

void HaLowInterface::startMeshInfoRequest()
{
#ifdef USE_MM_IOT_ESP32
    if (scanInProgress) {
        return;
    }

    size_t meshIdLen = strnlen(meshId, MMWLAN_SSID_MAXLEN);
    if (meshIdLen == 0) {
        return;
    }

    meshScanReq = MMWLAN_SCAN_REQ_INIT;
    meshScanIes[0] = WLAN_IE_ID_MESH_ID;
    meshScanIes[1] = (uint8_t)meshIdLen;
    memcpy(&meshScanIes[2], meshId, meshIdLen);

    meshScanReq.scan_rx_cb = scanRxTrampoline;
    meshScanReq.scan_complete_cb = scanCompleteTrampoline;
    meshScanReq.scan_cb_arg = this;
    meshScanReq.args.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
    memcpy(meshScanReq.args.ssid, meshId, meshIdLen);
    meshScanReq.args.ssid_len = meshIdLen;
    meshScanReq.args.extra_ies = meshScanIes;
    meshScanReq.args.extra_ies_len = 2 + meshIdLen;

    enum mmwlan_status st = mmwlan_scan_request(&meshScanReq);
    lastScanMs = millis();
    if (st == MMWLAN_SUCCESS) {
        scanInProgress = true;
        LOG_INFO("HaLow: requested mesh info id='%s'", meshId);
    } else {
        LOG_WARN("HaLow: mesh info request failed (%d)", (int)st);
    }
#endif
}

#ifdef USE_MM_IOT_ESP32
void HaLowInterface::onMeshScanResult(const struct mmwlan_scan_result *result)
{
    if (!result || !result->bssid || !result->ies) {
        return;
    }

    const uint8_t *meshId = nullptr;
    uint8_t meshIdLen = 0;
    bool hasMeshConfig = false;
    size_t off = 0;
    while (off + 2 <= result->ies_len) {
        uint8_t eid = result->ies[off];
        uint8_t len = result->ies[off + 1];
        size_t next = off + 2 + len;
        if (next > result->ies_len) {
            break;
        }
        if (eid == WLAN_IE_ID_MESH_ID) {
            meshId = &result->ies[off + 2];
            meshIdLen = len;
        } else if (eid == WLAN_IE_ID_MESH_CONFIG) {
            hasMeshConfig = true;
        }
        off = next;
    }

    size_t targetLen = strnlen(this->meshId, MMWLAN_SSID_MAXLEN);
    bool meshIdMatches = meshId && meshIdLen == targetLen && memcmp(meshId, this->meshId, targetLen) == 0;
    if (!meshIdMatches && !hasMeshConfig) {
        return;
    }

    meshPeerSeen = true;
    lastMeshInfoMs = millis();
    if (result->rssi > bestMeshRssi) {
        bestMeshRssi = result->rssi;
        memcpy(bestMeshBssid, result->bssid, sizeof(bestMeshBssid));
        size_t copyLen = meshIdLen < sizeof(bestMeshId) - 1 ? meshIdLen : sizeof(bestMeshId) - 1;
        if (meshId && copyLen > 0) {
            memcpy(bestMeshId, meshId, copyLen);
        }
        bestMeshId[copyLen] = '\0';
    }

    LOG_INFO("HaLow: mesh info id='%.*s' bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d mesh_cfg=%u",
             (int)meshIdLen, meshId ? (const char *)meshId : "", result->bssid[0], result->bssid[1], result->bssid[2],
             result->bssid[3], result->bssid[4], result->bssid[5], result->rssi, hasMeshConfig ? 1 : 0);
}

void HaLowInterface::onMeshScanComplete(enum mmwlan_scan_state scan_state)
{
    scanInProgress = false;
    LOG_INFO("HaLow: mesh info request complete state=%d seen=%u", (int)scan_state, meshPeerSeen ? 1 : 0);
}
#endif

void HaLowInterface::onFrameReceived(const uint8_t *payload, size_t payload_len, int8_t rssi)
{
    if (!payload || payload_len < sizeof(PacketHeader)) {
        return;
    }
    // Cap at our RadioBuffer size — anything larger is malformed for our wire
    // format and we drop it rather than corrupt memory.
    if (payload_len > sizeof(RadioBuffer)) {
        LOG_WARN("HaLow: rx %u > RadioBuffer, dropping", (unsigned)payload_len);
        return;
    }

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p) {
        return;
    }

    // Unpack the RadioBuffer into a MeshPacket (mirrors the LoRa RX decode).
    const PacketHeader *h = reinterpret_cast<const PacketHeader *>(payload);
    p->from = h->from;
    p->to = h->to;
    p->id = h->id;
    p->channel = h->channel;
    p->hop_limit = h->flags & PACKET_FLAGS_HOP_LIMIT_MASK;
    p->want_ack = !!(h->flags & PACKET_FLAGS_WANT_ACK_MASK);
    p->via_mqtt = !!(h->flags & PACKET_FLAGS_VIA_MQTT_MASK);
    p->hop_start = (h->flags & PACKET_FLAGS_HOP_START_MASK) >> PACKET_FLAGS_HOP_START_SHIFT;
    p->relay_node = h->relay_node;
    p->next_hop = h->next_hop;

    size_t payload_only = payload_len - sizeof(PacketHeader);
    if (payload_only > sizeof(p->encrypted.bytes)) {
        packetPool.release(p);
        return;
    }
    memcpy(p->encrypted.bytes, payload + sizeof(PacketHeader), payload_only);
    p->encrypted.size = payload_only;
    p->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;

    // mmwlan's rx callback doesn't carry per-frame RSSI on this SDK version —
    // fall back to the connection-level RSSI for visibility in the phone UI.
    (void)rssi;
    int32_t link_rssi = mmwlan_get_rssi();
    p->rx_rssi = (link_rssi == INT32_MIN) ? 0 : (int8_t)link_rssi;
    p->rx_snr = 0;
    p->rx_time = getValidTime(RTCQualityFromNet);

    deliverToReceiver(p);
}

#endif // USE_HALOW_RADIO
