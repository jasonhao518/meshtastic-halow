#include "configuration.h"
#ifdef USE_HALOW_RADIO

#include "HaLowFrame.h"
#include "HaLowInterface.h"
#include "Channels.h"
#include "MeshRadio.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "RTC.h" // getValidTime / RTCQualityFromNet
#include "Throttle.h"
#include "main.h"
#include "modules/NodeInfoModule.h"
#include <algorithm>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef USE_MM_IOT_ESP32
extern "C" {
#include "mmpkt.h"
#include "mmhal.h"
#include "mmregdb.h"
#include "mmwlan.h"
}
#include "driver/gpio.h"
#include "soc/gpio_reg.h"

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

static constexpr size_t HEX_PREVIEW_BYTES = 64;

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

static bool bytesNonzero(const uint8_t *bytes, size_t len)
{
    if (!bytes) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != 0) {
            return true;
        }
    }
    return false;
}

static void logHaLowMeshPacket(const char *direction, const uint8_t *buffer, size_t len, int rssi, bool hasRssi)
{
    if (!buffer || len < sizeof(PacketHeader)) {
        return;
    }

    const PacketHeader *h = reinterpret_cast<const PacketHeader *>(buffer);
    size_t payloadLen = len - sizeof(PacketHeader);
    size_t hexLen = std::min(len, HEX_PREVIEW_BYTES);
    char hex[(HEX_PREVIEW_BYTES * 2) + 1] = {0};
    char rssiText[12] = "n/a";
    bytesToHex(buffer, hexLen, hex, sizeof(hex));
    if (hasRssi) {
        snprintf(rssiText, sizeof(rssiText), "%d", rssi);
    }

    uint8_t hopLimit = h->flags & PACKET_FLAGS_HOP_LIMIT_MASK;
    uint8_t hopStart = (h->flags & PACKET_FLAGS_HOP_START_MASK) >> PACKET_FLAGS_HOP_START_SHIFT;
    LOG_INFO("HaLowPacket %s len=%u payload=%u from=0x%08x to=0x%08x id=0x%08x ch=%u flags=0x%02x hop=%u start=%u "
             "next=0x%02x relay=0x%02x rssi=%s hex%u=%s%s",
             direction, (unsigned)len, (unsigned)payloadLen, h->from, h->to, h->id, h->channel, h->flags, hopLimit,
             hopStart, h->next_hop, h->relay_node, rssiText, (unsigned)hexLen, hex, len > hexLen ? "..." : "");
    printf("HaLowPacket %s len=%u payload=%u from=0x%08x to=0x%08x id=0x%08x ch=%u flags=0x%02x hop=%u start=%u "
           "next=0x%02x relay=0x%02x rssi=%s hex%u=%s%s\n",
           direction, (unsigned)len, (unsigned)payloadLen, h->from, h->to, h->id, h->channel, h->flags, hopLimit, hopStart,
           h->next_hop, h->relay_node, rssiText, (unsigned)hexLen, hex, len > hexLen ? "..." : "");
}

#ifdef USE_MM_IOT_ESP32
struct HalowHwModelMacPrefix {
    uint8_t hwModel;
    uint8_t prefix[3];
};

static constexpr HalowHwModelMacPrefix halowHwModelMacPrefixes[] = {
    {meshtastic_HardwareModel_HELTEC_V3, {0xB4, 0x3A, 0x45}},
};

static bool getHalowMacPrefixForHwModel(uint8_t hwModel, uint8_t outPrefix[3])
{
    if (!outPrefix) {
        return false;
    }
    for (const auto &entry : halowHwModelMacPrefixes) {
        if (entry.hwModel == hwModel) {
            memcpy(outPrefix, entry.prefix, sizeof(entry.prefix));
            return true;
        }
    }
    return false;
}

static const char *staEventToStr(enum mmwlan_sta_event evt)
{
    switch (evt) {
    case MMWLAN_STA_EVT_SCAN_REQUEST:
        return "SCAN_REQUEST";
    case MMWLAN_STA_EVT_SCAN_COMPLETE:
        return "SCAN_COMPLETE";
    case MMWLAN_STA_EVT_SCAN_ABORT:
        return "SCAN_ABORT";
    case MMWLAN_STA_EVT_AUTH_REQUEST:
        return "AUTH_REQUEST";
    case MMWLAN_STA_EVT_ASSOC_REQUEST:
        return "ASSOC_REQUEST";
    case MMWLAN_STA_EVT_DEAUTH_TX:
        return "DEAUTH_TX";
    case MMWLAN_STA_EVT_CTRL_PORT_OPEN:
        return "CTRL_PORT_OPEN";
    case MMWLAN_STA_EVT_CTRL_PORT_CLOSED:
        return "CTRL_PORT_CLOSED";
    default:
        return "UNKNOWN";
    }
}
#endif

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
    const uint8_t *srcMac = header + 6;
    self->onFrameReceived(payload, payload_len, /*rssi*/ 0, srcMac, 6);
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

void HaLowInterface::staEventTrampoline(const struct mmwlan_sta_event_cb_args *sta_event, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (self) {
        self->onStaEvent(sta_event);
    }
}

void HaLowInterface::beaconVendorIeTrampoline(const uint8_t *ies, uint32_t ies_len, void *arg)
{
    HaLowInterface *self = static_cast<HaLowInterface *>(arg);
    if (self) {
        self->onDiscoveryVendorIes(ies, ies_len, 0);
    }
}
#endif

HaLowInterface::~HaLowInterface() = default;

bool HaLowInterface::init()
{
    printf("HaLow: init entry\n");
    RadioInterface::init();

#ifdef USE_MM_IOT_ESP32
    if (!loadMeshProfile()) {
        return false;
    }

    gpio_config_t irqPin = {};
    irqPin.pin_bit_mask = (1ULL << CONFIG_MM_SPI_IRQ) | (1ULL << CONFIG_MM_BUSY);
    irqPin.mode = GPIO_MODE_INPUT;
    irqPin.pull_up_en = GPIO_PULLUP_DISABLE;
    irqPin.pull_down_en = GPIO_PULLDOWN_DISABLE;
    irqPin.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&irqPin);
    gpio_intr_disable((gpio_num_t)CONFIG_MM_SPI_IRQ);
    gpio_intr_disable((gpio_num_t)CONFIG_MM_BUSY);
    REG_WRITE(GPIO_STATUS_W1TC_REG, UINT32_MAX);
    REG_WRITE(GPIO_STATUS1_W1TC_REG, UINT32_MAX);
    printf("HaLow: cleared pending Morse GPIO interrupts\n");

    LOG_INFO("HaLow: mmhal_init()");
    printf("HaLow: calling mmhal_init\n");
    fflush(stdout);
    mmhal_init();
    printf("HaLow: mmhal_init complete\n");
    fflush(stdout);

    LOG_INFO("HaLow: mmwlan_init()");
    printf("HaLow: calling mmwlan_init\n");
    fflush(stdout);
    mmwlan_init();
    printf("HaLow: mmwlan_init complete\n");
    fflush(stdout);

    if (!applyChannelList()) {
        return false;
    }

    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    printf("HaLow: calling mmwlan_boot\n");
    fflush(stdout);
    enum mmwlan_status st = mmwlan_boot(&boot_args);
    if (st != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: mmwlan_boot failed (%d) — firmware load or SPI wiring", (int)st);
        printf("HaLow: mmwlan_boot failed (%d)\n", (int)st);
        return false;
    }
    printf("HaLow: mmwlan_boot complete\n");
    wlanReady = true;

    struct mmwlan_version version;
    if (mmwlan_get_version(&version) == MMWLAN_SUCCESS) {
        LOG_INFO("HaLow: chip 0x%lx, fw %s, lib %s", (unsigned long)version.morse_chip_id, version.morse_fw_version,
                 version.morselib_version);
        printf("HaLow: chip 0x%lx, fw %s, lib %s\n", (unsigned long)version.morse_chip_id, version.morse_fw_version,
               version.morselib_version);
    }

    mmwlan_register_link_state_cb(linkStateTrampoline, this);
    if (mmwlan_register_rx_cb(rxTrampoline, this) != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: register_rx_cb failed");
        printf("HaLow: register_rx_cb failed\n");
        return false;
    }

    return startMeshStation();
#else
    LOG_WARN("HaLow: built without USE_MM_IOT_ESP32, transport is a stub");
    return false;
#endif
}

bool HaLowInterface::reconfigure()
{
#ifdef USE_MM_IOT_ESP32
    if (!wlanReady) {
        return false;
    }

    if (meshEnabled) {
        mmwlan_sta_disable();
        meshEnabled = false;
        linkUp = false;
        scanInProgress = false;
        nodeInfoPingPending = false;
    }

    if (!loadMeshProfile() || !applyChannelList()) {
        return false;
    }

    return startMeshStation();
#else
    return false;
#endif
}

bool HaLowInterface::sleep()
{
#ifdef USE_MM_IOT_ESP32
    if (meshEnabled) {
        mmwlan_sta_disable();
        meshEnabled = false;
        linkUp = false;
        scanInProgress = false;
        nodeInfoPingPending = false;
    }
#endif
    return true;
}

bool HaLowInterface::canSleep()
{
    return sendingPacket == nullptr;
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

    buildDiscoveryVendorIe();

    LOG_INFO("HaLow: profile from Meshtastic country=%s mesh_id='%s' key=%s", countryCode, meshId,
             meshKey[0] ? "primary-channel" : "open");
    return true;
}

void HaLowInterface::buildDiscoveryVendorIe()
{
    discoveryVendorIeLen = 0;
    uint8_t payload[MAX_DISCOVERY_VENDOR_IES * MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN] = {0};
    size_t payloadLen = 0;

    int16_t channelHash = channels.getHash(channels.getPrimaryIndex());
    NodeNum nodeNum = nodeDB ? nodeDB->getNodeNum() : 0;
    if (channelHash < 0 || nodeNum == 0) {
        return;
    }

    auto putByte = [&](uint8_t v) -> bool {
        if (payloadLen >= sizeof(payload)) {
            return false;
        }
        payload[payloadLen++] = v;
        return true;
    };
    auto putBytes = [&](const uint8_t *data, size_t len) -> bool {
        if (payloadLen + len > sizeof(payload)) {
            return false;
        }
        if (len > 0) {
            memcpy(payload + payloadLen, data, len);
        }
        payloadLen += len;
        return true;
    };
    auto putString = [&](const char *s, size_t maxLen) -> bool {
        size_t len = s ? strnlen(s, maxLen) : 0;
        if (len > 39) {
            len = 39;
        }
        return putByte((uint8_t)len) && putBytes((const uint8_t *)s, len);
    };
    auto putPublicKey = [&]() -> bool {
        if (owner.public_key.size == sizeof(owner.public_key.bytes) &&
            bytesNonzero(owner.public_key.bytes, owner.public_key.size)) {
            return putByte((uint8_t)owner.public_key.size) && putBytes(owner.public_key.bytes, owner.public_key.size);
        }
        return putByte(0);
    };

    if (!putByte((uint8_t)channelHash) || !putBytes((const uint8_t *)&nodeNum, sizeof(nodeNum)) ||
        !putByte((uint8_t)owner.hw_model) || !putByte((uint8_t)owner.role) || !putByte(owner.is_licensed ? 1 : 0) ||
        !putString(owner.short_name, sizeof(owner.short_name)) || !putString(owner.long_name, sizeof(owner.long_name)) ||
        !putPublicKey()) {
        return;
    }

    size_t fragCount = (payloadLen + MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN - 1) / MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN;
    if (fragCount == 0 || fragCount > MAX_DISCOVERY_VENDOR_IES) {
        return;
    }

    size_t src = 0;
    size_t outPos = 0;
    for (size_t frag = 0; frag < fragCount; frag++) {
        size_t fragPayloadLen = payloadLen - src;
        if (fragPayloadLen > MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN) {
            fragPayloadLen = MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN;
        }
        uint8_t *out = discoveryVendorIe + outPos;
        out[0] = WLAN_IE_ID_VENDOR_SPECIFIC;
        out[1] = (uint8_t)(MESHTASTIC_VENDOR_HEADER_LEN + fragPayloadLen);
        memcpy(out + 2, MESHTASTIC_VENDOR_OUI, sizeof(MESHTASTIC_VENDOR_OUI));
        out[5] = MESHTASTIC_VENDOR_TYPE_NODEINFO;
        out[6] = MESHTASTIC_VENDOR_VERSION;
        out[7] = (uint8_t)frag;
        out[8] = (uint8_t)fragCount;
        memcpy(out + 2 + MESHTASTIC_VENDOR_HEADER_LEN, payload + src, fragPayloadLen);
        src += fragPayloadLen;
        outPos += 2 + MESHTASTIC_VENDOR_HEADER_LEN + fragPayloadLen;
    }

    discoveryVendorIeLen = outPos;
    LOG_INFO("HaLow: Meshtastic vendor IE ready node=0x%08x hash=0x%02x len=%u key_len=%u", nodeNum, (uint8_t)channelHash,
             (unsigned)discoveryVendorIeLen, (unsigned)owner.public_key.size);
}

void HaLowInterface::onDiscoveryVendorIes(const uint8_t *ies, size_t iesLen, int8_t rssi, const uint8_t *srcMac, size_t srcMacLen)
{
    if (!ies) {
        printf("HaLow: vendor IE RX ignored null ies rssi=%d\n", rssi);
        return;
    }
    if (!nodeDB) {
        printf("HaLow: vendor IE RX ignored no nodeDB len=%u rssi=%d\n", (unsigned)iesLen, rssi);
        return;
    }

    char iesHex[(HEX_PREVIEW_BYTES * 2) + 1] = {0};
    size_t iesHexLen = std::min(iesLen, HEX_PREVIEW_BYTES);
    bytesToHex(ies, iesHexLen, iesHex, sizeof(iesHex));
    LOG_INFO("HaLow: vendor IE RX len=%u rssi=%d hex%u=%s%s", (unsigned)iesLen, rssi, (unsigned)iesHexLen, iesHex,
             iesLen > iesHexLen ? "..." : "");
    printf("HaLow: vendor IE RX len=%u rssi=%d hex%u=%s%s\n", (unsigned)iesLen, rssi, (unsigned)iesHexLen, iesHex,
           iesLen > iesHexLen ? "..." : "");

    uint8_t reassembled[MAX_DISCOVERY_VENDOR_IES * MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN] = {0};
    size_t fragLens[MAX_DISCOVERY_VENDOR_IES] = {0};
    bool fragSeen[MAX_DISCOVERY_VENDOR_IES] = {false};
    uint8_t expectedFrags = 0;

    size_t off = 0;
    while (off + 2 <= iesLen) {
        uint8_t eid = ies[off];
        uint8_t len = ies[off + 1];
        size_t next = off + 2 + len;
        if (next > iesLen) {
            LOG_WARN("HaLow: vendor IE malformed element off=%u eid=0x%02x len=%u ies_len=%u", (unsigned)off, eid, len,
                     (unsigned)iesLen);
            printf("HaLow: vendor IE malformed element off=%u eid=0x%02x len=%u ies_len=%u\n", (unsigned)off, eid, len,
                   (unsigned)iesLen);
            break;
        }
        const uint8_t *body = ies + off + 2;
        bool isVendor = eid == WLAN_IE_ID_VENDOR_SPECIFIC && len >= MESHTASTIC_VENDOR_HEADER_LEN;
        bool ouiMatch = isVendor && memcmp(body, MESHTASTIC_VENDOR_OUI, sizeof(MESHTASTIC_VENDOR_OUI)) == 0;
        uint8_t vendorType = isVendor ? body[3] : 0;
        uint8_t vendorVersion = isVendor ? body[4] : 0;
        uint8_t fragIndex = isVendor ? body[5] : 0;
        uint8_t fragCount = isVendor ? body[6] : 0;
        size_t fragPayloadLen = isVendor ? len - MESHTASTIC_VENDOR_HEADER_LEN : 0;

        if (isVendor) {
            LOG_INFO("HaLow: vendor IE elem off=%u len=%u oui=%u type=%u version=%u frag=%u/%u payload=%u", (unsigned)off,
                     len, ouiMatch ? 1 : 0, vendorType, vendorVersion, fragIndex, fragCount, (unsigned)fragPayloadLen);
            printf("HaLow: vendor IE elem off=%u len=%u oui=%u type=%u version=%u frag=%u/%u payload=%u\n", (unsigned)off,
                   len, ouiMatch ? 1 : 0, vendorType, vendorVersion, fragIndex, fragCount, (unsigned)fragPayloadLen);
        }

        if (ouiMatch && vendorType == MESHTASTIC_VENDOR_TYPE_NODEINFO && vendorVersion == MESHTASTIC_VENDOR_VERSION) {
            if (fragCount > 0 && fragCount <= MAX_DISCOVERY_VENDOR_IES && fragIndex < fragCount &&
                fragPayloadLen <= MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN) {
                expectedFrags = fragCount;
                memcpy(reassembled + (fragIndex * MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN),
                       body + MESHTASTIC_VENDOR_HEADER_LEN, fragPayloadLen);
                fragLens[fragIndex] = fragPayloadLen;
                fragSeen[fragIndex] = true;
            } else {
                LOG_WARN("HaLow: vendor IE reject bad fragment frag=%u/%u payload=%u max_frags=%u max_payload=%u", fragIndex,
                         fragCount, (unsigned)fragPayloadLen, (unsigned)MAX_DISCOVERY_VENDOR_IES,
                         (unsigned)MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN);
                printf("HaLow: vendor IE reject bad fragment frag=%u/%u payload=%u max_frags=%u max_payload=%u\n", fragIndex,
                       fragCount, (unsigned)fragPayloadLen, (unsigned)MAX_DISCOVERY_VENDOR_IES,
                       (unsigned)MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN);
            }
        } else if (isVendor) {
            LOG_INFO("HaLow: vendor IE skip oui=%u type=%u version=%u", ouiMatch ? 1 : 0, vendorType, vendorVersion);
            printf("HaLow: vendor IE skip oui=%u type=%u version=%u\n", ouiMatch ? 1 : 0, vendorType, vendorVersion);
        }
        off = next;
    }

    if (expectedFrags == 0) {
        LOG_INFO("HaLow: vendor IE RX no Meshtastic NodeInfo fragment len=%u", (unsigned)iesLen);
        printf("HaLow: vendor IE RX no Meshtastic NodeInfo fragment len=%u\n", (unsigned)iesLen);
        return;
    }

    for (uint8_t i = 0; i < expectedFrags; i++) {
        if (!fragSeen[i]) {
            LOG_WARN("HaLow: Meshtastic vendor IE missing fragment %u/%u", (unsigned)i, (unsigned)expectedFrags);
            return;
        }
        if (i + 1 < expectedFrags && fragLens[i] != MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN) {
            LOG_WARN("HaLow: Meshtastic vendor IE short middle fragment %u len=%u", (unsigned)i, (unsigned)fragLens[i]);
            return;
        }
    }

    uint8_t compact[MAX_DISCOVERY_VENDOR_IES * MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN] = {0};
    size_t compactLen = 0;
    for (uint8_t i = 0; i < expectedFrags; i++) {
        memcpy(compact + compactLen, reassembled + (i * MESHTASTIC_VENDOR_FRAGMENT_PAYLOAD_LEN), fragLens[i]);
        compactLen += fragLens[i];
    }

    char compactHex[(HEX_PREVIEW_BYTES * 2) + 1] = {0};
    size_t compactHexLen = std::min(compactLen, HEX_PREVIEW_BYTES);
    bytesToHex(compact, compactHexLen, compactHex, sizeof(compactHex));
    LOG_INFO("HaLow: vendor IE compact len=%u hex%u=%s%s", (unsigned)compactLen, (unsigned)compactHexLen, compactHex,
             compactLen > compactHexLen ? "..." : "");
    printf("HaLow: vendor IE compact len=%u hex%u=%s%s\n", (unsigned)compactLen, (unsigned)compactHexLen, compactHex,
           compactLen > compactHexLen ? "..." : "");

    size_t pos = 0;
    auto getByte = [&](uint8_t &v) -> bool {
        if (pos >= compactLen) {
            return false;
        }
        v = compact[pos++];
        return true;
    };
    auto getBytes = [&](uint8_t *out, size_t len) -> bool {
        if (pos + len > compactLen) {
            return false;
        }
        memcpy(out, compact + pos, len);
        pos += len;
        return true;
    };
    auto getString = [&](char *out, size_t outLen) -> bool {
        uint8_t len = 0;
        if (!getByte(len) || len >= outLen || pos + len > compactLen) {
            return false;
        }
        memcpy(out, compact + pos, len);
        out[len] = '\0';
        pos += len;
        return true;
    };
    auto getOptionalPublicKey = [&](meshtastic_User &u) -> bool {
        if (pos >= compactLen) {
            return true;
        }

        uint8_t len = 0;
        if (!getByte(len) || len > sizeof(u.public_key.bytes) || pos + len > compactLen) {
            return false;
        }
        if (len != 0 && len != sizeof(u.public_key.bytes)) {
            return false;
        }
        u.public_key.size = len;
        if (len > 0) {
            memcpy(u.public_key.bytes, compact + pos, len);
            pos += len;
        }
        return true;
    };

    uint8_t channelHash = 0;
    NodeNum nodeNum = 0;
    uint8_t hwModel = 0;
    uint8_t role = 0;
    uint8_t flags = 0;
    meshtastic_User user = meshtastic_User_init_default;

    if (!getByte(channelHash) || !getBytes((uint8_t *)&nodeNum, sizeof(nodeNum)) || !getByte(hwModel) || !getByte(role) ||
        !getByte(flags) || !getString(user.short_name, sizeof(user.short_name)) || !getString(user.long_name, sizeof(user.long_name)) ||
        !getOptionalPublicKey(user)) {
        LOG_WARN("HaLow: malformed Meshtastic vendor IE payload len=%u pos=%u", (unsigned)compactLen, (unsigned)pos);
        printf("HaLow: malformed Meshtastic vendor IE payload len=%u pos=%u\n", (unsigned)compactLen, (unsigned)pos);
        return;
    }

    int16_t localHash = channels.getHash(channels.getPrimaryIndex());
    LOG_INFO("HaLow: vendor NodeInfo decoded hash=0x%02x local=0x%02x node=0x%08x hw=%u role=%u flags=0x%02x key_len=%u short='%s' long='%s'",
             channelHash, localHash < 0 ? 0xff : (uint8_t)localHash, nodeNum, hwModel, role, flags, user.public_key.size,
             user.short_name, user.long_name);
    printf("HaLow: vendor NodeInfo decoded hash=0x%02x local=0x%02x node=0x%08x hw=%u role=%u flags=0x%02x key_len=%u short='%s' long='%s'\n",
           channelHash, localHash < 0 ? 0xff : (uint8_t)localHash, nodeNum, hwModel, role, flags, user.public_key.size,
           user.short_name, user.long_name);

    if (localHash < 0 || channelHash != (uint8_t)localHash) {
        LOG_INFO("HaLow: ignore vendor NodeInfo hash=0x%02x local=0x%02x", channelHash, localHash < 0 ? 0xff : (uint8_t)localHash);
        printf("HaLow: ignore vendor NodeInfo hash=0x%02x local=0x%02x\n", channelHash,
               localHash < 0 ? 0xff : (uint8_t)localHash);
        return;
    }
    if (nodeNum == 0 || nodeNum == nodeDB->getNodeNum()) {
        LOG_INFO("HaLow: ignore vendor NodeInfo node=0x%08x local=0x%08x", nodeNum, nodeDB->getNodeNum());
        printf("HaLow: ignore vendor NodeInfo node=0x%08x local=0x%08x\n", nodeNum, nodeDB->getNodeNum());
        return;
    }

    snprintf(user.id, sizeof(user.id), "!%08x", nodeNum);
    user.hw_model = (meshtastic_HardwareModel)hwModel;
    user.role = (meshtastic_Config_DeviceConfig_Role)role;
    user.is_licensed = (flags & 0x01) != 0;

    bool changed = nodeDB->updateUser(nodeNum, user, channels.getPrimaryIndex());
    meshPeerSeen = true;
    nodeInfoPingPending = true;
    lastMeshInfoMs = millis();
    if (srcMacLen >= 6) {
        cachePeerMac(nodeNum, srcMac, hwModel);
    }
    LOG_INFO("HaLow: vendor NodeInfo %s node=0x%08x short='%s' long='%s' rssi=%d", changed ? "updated" : "seen",
             nodeNum, user.short_name, user.long_name, rssi);
    printf("HaLow: vendor NodeInfo %s node=0x%08x short='%s' long='%s' rssi=%d\n", changed ? "updated" : "seen", nodeNum,
           user.short_name, user.long_name, rssi);
}

#ifdef USE_MM_IOT_ESP32
bool HaLowInterface::applyChannelList()
{
    const struct mmwlan_s1g_channel_list *channel_list = mmwlan_lookup_regulatory_domain(get_regulatory_db(), countryCode);
    if (!channel_list) {
        LOG_ERROR("HaLow: country %s not in regdb", countryCode);
        printf("HaLow: country %s not in regdb\n", countryCode);
        return false;
    }
    if (mmwlan_set_channel_list(channel_list) != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: set_channel_list failed");
        printf("HaLow: set_channel_list failed\n");
        return false;
    }
    printf("HaLow: regulatory domain %s set\n", countryCode);
    return true;
}

bool HaLowInterface::startMeshStation()
{
    struct mmwlan_scan_config scanConfig = MMWLAN_SCAN_CONFIG_INIT;
    scanConfig.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
    scanConfig.home_channel_dwell_time_ms = 0;
    if (mmwlan_set_scan_config(&scanConfig) != MMWLAN_SUCCESS) {
        LOG_WARN("HaLow: set mesh scan config failed");
    }

    struct mmwlan_sta_args staArgs = MMWLAN_STA_ARGS_INIT;
    staArgs.ssid_len = strnlen(meshId, sizeof(staArgs.ssid));
    memcpy(staArgs.ssid, meshId, staArgs.ssid_len);
    staArgs.passphrase_len = 0;
    staArgs.security_type = MMWLAN_OPEN;
    staArgs.scan_rx_cb = scanRxTrampoline;
    staArgs.scan_rx_cb_arg = this;
    staArgs.sta_evt_cb = staEventTrampoline;
    staArgs.sta_evt_cb_arg = this;
    staArgs.bgscan_short_interval_s = 0;
    staArgs.bgscan_long_interval_s = 0;
    staArgs.scan_interval_base_s = MESH_CONNECT_SCAN_BASE_S;
    staArgs.scan_interval_limit_s = MESH_CONNECT_SCAN_LIMIT_S;
    staArgs.extra_assoc_ies = discoveryVendorIeLen ? discoveryVendorIe : nullptr;
    staArgs.extra_assoc_ies_len = discoveryVendorIeLen;
    staArgs.mesh_mode = true;

    enum mmwlan_status meshStatus = mmwlan_sta_enable(&staArgs, NULL);
    if (meshStatus != MMWLAN_SUCCESS) {
        LOG_ERROR("HaLow: mesh STA enable failed (%d)", (int)meshStatus);
        printf("HaLow: mesh STA enable failed (%d)\n", (int)meshStatus);
        return false;
    }

    meshEnabled = true;
    meshPeerSeen = false;
    nodeInfoPingPending = false;
    staEventCount = 0;
    staScanCount = 0;
    staScanResultCount = 0;
    staMeshAdvSeenCount = 0;
    staTargetIdHitCount = 0;
    staAuthReqCount = 0;
    staAssocReqCount = 0;
    staCtrlPortOpenCount = 0;
    bestMeshRssi = -32768;
    bestMeshId[0] = '\0';

    memset(&beaconVendorIeFilter, 0, sizeof(beaconVendorIeFilter));
    beaconVendorIeFilter.cb = beaconVendorIeTrampoline;
    beaconVendorIeFilter.cb_arg = this;
    beaconVendorIeFilter.n_ouis = 1;
    memcpy(beaconVendorIeFilter.ouis[0], MESHTASTIC_VENDOR_OUI, sizeof(MESHTASTIC_VENDOR_OUI));
    enum mmwlan_status filterStatus = mmwlan_update_beacon_vendor_ie_filter(&beaconVendorIeFilter);
    if (filterStatus != MMWLAN_SUCCESS) {
        LOG_WARN("HaLow: beacon vendor IE filter failed (%d)", (int)filterStatus);
        printf("HaLow: beacon vendor IE filter failed (%d)\n", (int)filterStatus);
    }

    LOG_INFO("HaLow: raw 802.11ah bearer enabled id='%s' country=%s wifi_key=open meshtastic_key=%s", meshId, countryCode,
             meshKey[0] ? "primary-psk" : "open");
    printf("HaLow: raw 802.11ah bearer enabled id='%s' country=%s wifi_key=open meshtastic_key=%s\n", meshId, countryCode,
           meshKey[0] ? "primary-psk" : "open");
    printf("HaLow: mesh internal scan retry base=%us limit=%us; app local scan still triggers immediate scan\n",
           (unsigned)staArgs.scan_interval_base_s, (unsigned)staArgs.scan_interval_limit_s);
    printf("HaLow: MESH_ADVERTISER enabled, waiting for beacon/probe-response callbacks\n");
    return true;
}
#endif

ErrorCode HaLowInterface::send(meshtastic_MeshPacket *p)
{
    if (!p) {
        return ERRNO_UNKNOWN;
    }

#ifdef USE_MM_IOT_ESP32
    if (!meshEnabled || disabled || !config.lora.tx_enabled) {
        LOG_WARN("HaLow: drop tx id=0x%08x mesh=%u disabled=%u tx_enabled=%u", p->id, meshEnabled ? 1 : 0, disabled ? 1 : 0,
                 config.lora.tx_enabled ? 1 : 0);
        packetPool.release(p);
        return (!meshEnabled || disabled) ? ERRNO_DISABLED : ERRNO_UNKNOWN;
    }

    // beginSending() serializes the MeshPacket into radioBuffer (PacketHeader
    // + payload). We then prepend a 14-byte 802.3 header for mmwlan.
    size_t encoded = beginSending(p);
    if (encoded == 0) {
        sendingPacket = NULL;
        packetPool.release(p);
        return ERRNO_UNKNOWN;
    }

    uint8_t txbuf[sizeof(HaLowEthFrameHeader) + sizeof(RadioBuffer)];
    if (encoded > sizeof(RadioBuffer)) {
        LOG_ERROR("HaLow: encoded %u > radioBuffer", (unsigned)encoded);
        sendingPacket = NULL;
        packetPool.release(p);
        return ERRNO_UNKNOWN;
    }

    logHaLowMeshPacket("tx", reinterpret_cast<const uint8_t *>(&radioBuffer), encoded, 0, false);

    NodeNum resolvedNodeNum = 0;
    if (resolveUnicastMac(p, txbuf, resolvedNodeNum)) {
        LOG_INFO("HaLow: tx unicast node=0x%08x via node=0x%08x", p->to, resolvedNodeNum);
    } else {
        // Keep existing mesh behavior if the destination map is incomplete.
        memcpy(txbuf, HALOW_BROADCAST_MAC, 6);
    }

    if (mmwlan_get_mac_addr(txbuf + 6) != MMWLAN_SUCCESS) {
        memset(txbuf + 6, 0, 6); // fallback so the frame still goes out
    }
    txbuf[12] = (uint8_t)(ETHERTYPE_MESHTASTIC_HALOW >> 8);
    txbuf[13] = (uint8_t)(ETHERTYPE_MESHTASTIC_HALOW & 0xFF);
    memcpy(txbuf + sizeof(HaLowEthFrameHeader), &radioBuffer, encoded);

    enum mmwlan_status st = mmwlan_tx_wait_until_ready(MMWLAN_TX_DEFAULT_TIMEOUT_MS);
    if (st == MMWLAN_SUCCESS) {
        struct mmwlan_tx_metadata metadata = MMWLAN_TX_METADATA_INIT;
        metadata.vif = MMWLAN_VIF_STA;
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
    if (st == MMWLAN_SUCCESS) {
        LOG_DEBUG("HaLow: tx queued id=0x%08x len=%u", p->id, (unsigned)(sizeof(HaLowEthFrameHeader) + encoded));
        airTime->logAirtime(TX_LOG, RadioInterface::getPacketTime(p));
    } else {
        LOG_WARN("HaLow: tx failed id=0x%08x status=%d", p->id, (int)st);
        printf("HaLow: tx failed id=0x%08x status=%d\n", p->id, (int)st);
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
    qs.free = meshEnabled ? 16 : 0;
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

bool HaLowInterface::requestLocalMeshScan()
{
#ifdef USE_MM_IOT_ESP32
    LOG_INFO("HaLow: local mesh scan requested by client");
    printf("HaLow: local mesh scan requested by client\n");
    return startMeshInfoRequest();
#else
    return false;
#endif
}

#ifdef USE_MM_IOT_ESP32
bool HaLowInterface::getLocalHalowMac(uint8_t outMac[6]) const
{
    return mmwlan_get_mac_addr(outMac) == MMWLAN_SUCCESS;
}

bool HaLowInterface::findHalowMacForNode(NodeNum nodeNum, uint8_t outMac[6]) const
{
    if (nodeNum == 0) {
        return false;
    }
    if (nodeDB && nodeNum == nodeDB->getNodeNum()) {
        return getLocalHalowMac(outMac);
    }
    auto it = peerMacCache.find(nodeNum);
    if (it != peerMacCache.end()) {
        memcpy(outMac, it->second.mac, 6);
        return true;
    }

    meshtastic_NodeInfoLite *peer = nodeDB ? nodeDB->getMeshNode(nodeNum) : nullptr;
    if (!peer) {
        return false;
    }
    return findHalowMacForNodeAlias(nodeNum, peer->hw_model, outMac);
}

bool HaLowInterface::deriveHalowMacFromNode(NodeNum nodeNum, uint8_t hwModel, uint8_t outMac[6]) const
{
    if (nodeNum == 0) {
        return false;
    }

    if (!outMac) {
        return false;
    }

    uint8_t prefix[3];
    if (getHalowMacPrefixForHwModel(hwModel, prefix)) {
        memcpy(outMac, prefix, sizeof(prefix));
        outMac[3] = (uint8_t)(nodeNum >> 16);
        outMac[4] = (uint8_t)(nodeNum >> 8);
        outMac[5] = (uint8_t)nodeNum;
        return true;
    }

    // Prefix bytes carry the model identity for deterministic reverse lookup.
    outMac[0] = 0x02; // Locally-administered, unicast.
    outMac[1] = hwModel;
    outMac[2] = (uint8_t)(nodeNum >> 24);
    outMac[3] = (uint8_t)(nodeNum >> 16);
    outMac[4] = (uint8_t)(nodeNum >> 8);
    outMac[5] = (uint8_t)nodeNum;
    return true;
}

bool HaLowInterface::findHalowMacForNodeAlias(NodeNum nodeNum, uint8_t hwModel, uint8_t outMac[6]) const
{
    if (nodeNum == 0) {
        return false;
    }
    uint8_t nodeIdLow = nodeDB ? nodeDB->getLastByteOfNodeNum(nodeNum) : (uint8_t)(nodeNum & 0xFF);
    if (nodeIdLow == 0) {
        nodeIdLow = 0xFF;
    }
    auto it = peerAliasCache.find(makeAliasKey(hwModel, nodeIdLow));
    if (it == peerAliasCache.end()) {
        return deriveHalowMacFromNode(nodeNum, hwModel, outMac);
    }
    memcpy(outMac, it->second.mac, 6);
    return true;
}

bool HaLowInterface::findHalowMacForNextHop(uint8_t nextHop, uint8_t outMac[6], NodeNum *resolvedNode) const
{
    if (resolvedNode) {
        *resolvedNode = 0;
    }
    if (nextHop == NO_NEXT_HOP_PREFERENCE) {
        return false;
    }
    if (nodeDB && nextHop != NO_NEXT_HOP_PREFERENCE && nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum()) == nextHop) {
        if (getLocalHalowMac(outMac)) {
            if (resolvedNode) {
                *resolvedNode = nodeDB->getNodeNum();
            }
            return true;
        }
    }

    NodeNum bestNode = 0;
    bool found = false;
    uint32_t nowMs = millis();
    uint32_t bestAge = 0;
    for (const auto &entry : peerMacCache) {
        if (nodeDB && nodeDB->getLastByteOfNodeNum(entry.first) == nextHop) {
            uint32_t age = entry.second.lastSeenMs == 0 ? UINT32_MAX : nowMs - entry.second.lastSeenMs;
            if (!found || age < bestAge) {
                memcpy(outMac, entry.second.mac, 6);
                bestNode = entry.first;
                bestAge = age;
                found = true;
            }
        }
    }
    for (const auto &entry : peerAliasCache) {
        uint8_t aliasLow = (uint8_t)(entry.first & 0x00FF);
        if (aliasLow != nextHop) {
            continue;
        }
        uint32_t age = entry.second.lastSeenMs == 0 ? UINT32_MAX : nowMs - entry.second.lastSeenMs;
        if (!found || age < bestAge) {
            memcpy(outMac, entry.second.mac, 6);
            bestNode = 0;
            bestAge = age;
            found = true;
        }
    }
    if (found) {
        if (resolvedNode) {
            *resolvedNode = bestNode;
        }
        return true;
    }

    if (nodeDB) {
        NodeNum bestCandidate = 0;
        uint32_t bestLastHeard = 0;
        bool candidateFound = false;

        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            const meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node || node->num == nodeDB->getNodeNum()) {
                continue;
            }
            if (nodeDB->getLastByteOfNodeNum(node->num) != nextHop) {
                continue;
            }
            uint8_t hwModel = node->hw_model != 0 ? (uint8_t)node->hw_model : (uint8_t)(node->num >> 24);
            if (!deriveHalowMacFromNode(node->num, hwModel, outMac)) {
                continue;
            }
            if (!candidateFound || node->last_heard > bestLastHeard) {
                bestCandidate = node->num;
                bestLastHeard = node->last_heard;
                candidateFound = true;
            }
        }

        if (candidateFound) {
            if (resolvedNode) {
                *resolvedNode = bestCandidate;
            }
            return deriveHalowMacFromNode(bestCandidate, (uint8_t)(bestCandidate >> 24), outMac);
        }
    }
    return false;
}

uint16_t HaLowInterface::makeAliasKey(uint8_t hardwareId, uint8_t nodeIdLowByte)
{
    return (uint16_t)(((uint16_t)hardwareId << 8) | nodeIdLowByte);
}

void HaLowInterface::cachePeerMac(NodeNum nodeNum, const uint8_t *mac, uint8_t hwModel)
{
    if (nodeNum == 0 || !mac) {
        return;
    }
    if (nodeNum == NODENUM_BROADCAST) {
        return;
    }
    PeerMacCacheEntry &entry = peerMacCache[nodeNum];
    memcpy(entry.mac, mac, 6);
    entry.lastSeenMs = millis();

    if (hwModel == 0 && nodeDB && nodeNum != nodeDB->getNodeNum()) {
        meshtastic_NodeInfoLite *peer = nodeDB ? nodeDB->getMeshNode(nodeNum) : nullptr;
        if (peer) {
            hwModel = peer->hw_model;
        }
    }
    if (hwModel == 0) {
        return;
    }
    uint8_t nodeIdLow = nodeDB ? nodeDB->getLastByteOfNodeNum(nodeNum) : 0;
    if (nodeIdLow == 0) {
        return;
    }
    PeerMacCacheEntry &aliasEntry = peerAliasCache[makeAliasKey(hwModel, nodeIdLow)];
    memcpy(aliasEntry.mac, mac, 6);
    aliasEntry.lastSeenMs = entry.lastSeenMs;
}

bool HaLowInterface::resolveUnicastMac(const meshtastic_MeshPacket *p, uint8_t outMac[6], NodeNum &resolvedNodeNum) const
{
    resolvedNodeNum = 0;
    if (!p || p->to == NODENUM_BROADCAST || p->to == 0) {
        return false;
    }

    if (findHalowMacForNextHop(p->next_hop, outMac, &resolvedNodeNum)) {
        return true;
    }

    if (findHalowMacForNode(p->to, outMac)) {
        resolvedNodeNum = p->to;
        return true;
    }

    if (deriveHalowMacFromNode(p->to, (uint8_t)(p->to >> 24), outMac)) {
        resolvedNodeNum = p->to;
        return true;
    }
    return false;
}
#endif

int32_t HaLowInterface::runOnce()
{
#ifdef USE_MM_IOT_ESP32
    if (!Throttle::isWithinTimespanMs(lastMeshStatusLogMs, MESH_STATUS_LOG_INTERVAL_MS)) {
        lastMeshStatusLogMs = millis();
        LOG_INFO("HaLow: mesh status beaconing=%u scan_in_progress=%u peer_seen=%u last_scan_age=%lu last_info_age=%lu",
                 meshEnabled ? 1 : 0, scanInProgress ? 1 : 0, meshPeerSeen ? 1 : 0,
                 (unsigned long)(millis() - lastScanMs), (unsigned long)(millis() - lastMeshInfoMs));
        printf("HaLow: mesh status beaconing=%u scan_in_progress=%u peer_seen=%u last_scan_age=%lums last_info_age=%lums "
               "sta_events=%lu sta_scans=%lu scan_results=%lu mesh_adv=%lu target_hits=%lu auth=%lu assoc=%lu ctrl_open=%lu\n",
               meshEnabled ? 1 : 0, scanInProgress ? 1 : 0, meshPeerSeen ? 1 : 0,
               (unsigned long)(millis() - lastScanMs), (unsigned long)(millis() - lastMeshInfoMs),
               (unsigned long)staEventCount, (unsigned long)staScanCount, (unsigned long)staScanResultCount,
               (unsigned long)staMeshAdvSeenCount, (unsigned long)staTargetIdHitCount, (unsigned long)staAuthReqCount,
               (unsigned long)staAssocReqCount, (unsigned long)staCtrlPortOpenCount);
    }

    bool nodeInfoIntervalReady = !Throttle::isWithinTimespanMs(lastNodeInfoPingMs, NODEINFO_PING_INTERVAL_MS);
    if (meshEnabled && nodeInfoModule && nodeInfoIntervalReady) {
        const char *reason = nodeInfoPingPending ? "beacon" : "periodic";
        nodeInfoPingPending = false;
        lastNodeInfoPingMs = millis();
        LOG_INFO("HaLow: sending NodeInfo discovery reason=%s", reason);
        printf("HaLow: sending NodeInfo discovery reason=%s id='%s' best_bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d\n",
               reason, bestMeshId, bestMeshBssid[0], bestMeshBssid[1], bestMeshBssid[2], bestMeshBssid[3],
               bestMeshBssid[4], bestMeshBssid[5], bestMeshRssi);
        nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
    }
#endif
    return 1000;
}

bool HaLowInterface::startMeshInfoRequest()
{
#ifdef USE_MM_IOT_ESP32
    if (!meshEnabled) {
        LOG_WARN("HaLow: mesh info request skipped, mesh not enabled");
        printf("HaLow: mesh info request skipped, mesh not enabled\n");
        return false;
    }

    if (scanInProgress) {
        LOG_INFO("HaLow: mesh info request skipped, scan already in progress");
        printf("HaLow: mesh info request skipped, scan already in progress\n");
        return false;
    }

    size_t meshIdLen = strnlen(meshId, MMWLAN_SSID_MAXLEN);
    if (meshIdLen == 0) {
        LOG_WARN("HaLow: mesh info request skipped, empty mesh id");
        printf("HaLow: mesh info request skipped, empty mesh id\n");
        return false;
    }

    meshScanReq = MMWLAN_SCAN_REQ_INIT;
    meshScanIes[0] = WLAN_IE_ID_MESH_ID;
    meshScanIes[1] = (uint8_t)meshIdLen;
    memcpy(&meshScanIes[2], meshId, meshIdLen);
    size_t meshScanIesLen = 2 + meshIdLen;
    if (discoveryVendorIeLen > 0 && meshScanIesLen + discoveryVendorIeLen <= sizeof(meshScanIes)) {
        memcpy(meshScanIes + meshScanIesLen, discoveryVendorIe, discoveryVendorIeLen);
        meshScanIesLen += discoveryVendorIeLen;
    }

    meshScanReq.scan_rx_cb = scanRxTrampoline;
    meshScanReq.scan_complete_cb = scanCompleteTrampoline;
    meshScanReq.scan_cb_arg = this;
    meshScanReq.args.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
    memcpy(meshScanReq.args.ssid, meshId, meshIdLen);
    meshScanReq.args.ssid_len = meshIdLen;
    meshScanReq.args.extra_ies = meshScanIes;
    meshScanReq.args.extra_ies_len = meshScanIesLen;

    LOG_INFO("HaLow: starting mesh info request id='%s' dwell=%ums extra_ies=%u", meshId,
             (unsigned)HALOW_MESH_SCAN_DWELL_MS, (unsigned)meshScanReq.args.extra_ies_len);
    printf("HaLow: starting mesh info request id='%s' dwell=%ums extra_ies=%u\n", meshId,
           (unsigned)HALOW_MESH_SCAN_DWELL_MS, (unsigned)meshScanReq.args.extra_ies_len);

    enum mmwlan_status st = mmwlan_scan_request(&meshScanReq);
    lastScanMs = millis();
    if (st == MMWLAN_SUCCESS) {
        scanInProgress = true;
        LOG_INFO("HaLow: requested mesh info id='%s'", meshId);
        printf("HaLow: requested mesh info id='%s'\n", meshId);
        return true;
    } else {
        LOG_WARN("HaLow: mesh info request failed (%d)", (int)st);
        printf("HaLow: mesh info request failed (%d)\n", (int)st);
        return false;
    }
#else
    return false;
#endif
}

#ifdef USE_MM_IOT_ESP32
void HaLowInterface::onStaEvent(const struct mmwlan_sta_event_cb_args *sta_event)
{
    if (!sta_event) {
        LOG_WARN("HaLow: STA_EVT missing payload");
        printf("HaLow: STA_EVT missing payload\n");
        return;
    }

    staEventCount++;
    switch (sta_event->event) {
    case MMWLAN_STA_EVT_SCAN_REQUEST:
        staScanCount++;
        break;
    case MMWLAN_STA_EVT_SCAN_COMPLETE:
        LOG_INFO("HaLow: STA scan summary req=%lu results=%lu target_hits=%lu mesh_adv=%lu",
                 (unsigned long)staScanCount, (unsigned long)staScanResultCount, (unsigned long)staTargetIdHitCount,
                 (unsigned long)staMeshAdvSeenCount);
        printf("HaLow: STA scan summary req=%lu results=%lu target_hits=%lu mesh_adv=%lu\n",
               (unsigned long)staScanCount, (unsigned long)staScanResultCount, (unsigned long)staTargetIdHitCount,
               (unsigned long)staMeshAdvSeenCount);
        break;
    case MMWLAN_STA_EVT_SCAN_ABORT:
        break;
    case MMWLAN_STA_EVT_AUTH_REQUEST:
        staAuthReqCount++;
        if ((staAuthReqCount % 4) == 0) {
            LOG_WARN("HaLow: repeated mesh auth without assoc, check mesh key/channel");
            printf("HaLow: repeated mesh auth without assoc, check mesh key/channel\n");
        }
        break;
    case MMWLAN_STA_EVT_ASSOC_REQUEST:
        staAssocReqCount++;
        break;
    case MMWLAN_STA_EVT_CTRL_PORT_OPEN:
        staCtrlPortOpenCount++;
        break;
    default:
        break;
    }

    LOG_INFO("HaLow: STA_EVT %s (%u)", staEventToStr(sta_event->event), (unsigned)sta_event->event);
    printf("HaLow: STA_EVT %s (%u)\n", staEventToStr(sta_event->event), (unsigned)sta_event->event);
}

void HaLowInterface::onMeshScanResult(const struct mmwlan_scan_result *result)
{
    if (!result || !result->bssid || !result->ies) {
        LOG_WARN("HaLow: scan result missing fields result=%p", result);
        printf("HaLow: scan result missing fields\n");
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
    staScanResultCount++;
    onDiscoveryVendorIes(result->ies, result->ies_len, result->rssi, result->bssid, 6);
    LOG_INFO("HaLow: scan result bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d ies=%u mesh_id='%.*s' match=%u mesh_cfg=%u",
             result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5],
             result->rssi, (unsigned)result->ies_len, (int)meshIdLen, meshId ? (const char *)meshId : "",
             meshIdMatches ? 1 : 0, hasMeshConfig ? 1 : 0);
    printf("HaLow: scan result bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d ies=%u mesh_id='%.*s' match=%u mesh_cfg=%u\n",
           result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5],
           result->rssi, (unsigned)result->ies_len, (int)meshIdLen, meshId ? (const char *)meshId : "", meshIdMatches ? 1 : 0,
           hasMeshConfig ? 1 : 0);
    if (!meshIdMatches && !hasMeshConfig) {
        return;
    }

    meshPeerSeen = true;
    nodeInfoPingPending = true;
    staMeshAdvSeenCount++;
    if (meshIdMatches) {
        staTargetIdHitCount++;
    }
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

    LOG_INFO("HaLow: remote mesh beacon id='%.*s' bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d mesh_cfg=%u",
             (int)meshIdLen, meshId ? (const char *)meshId : "", result->bssid[0], result->bssid[1], result->bssid[2],
             result->bssid[3], result->bssid[4], result->bssid[5], result->rssi, hasMeshConfig ? 1 : 0);
    printf("HaLow: remote mesh beacon id='%.*s' bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d mesh_cfg=%u\n",
           (int)meshIdLen, meshId ? (const char *)meshId : "", result->bssid[0], result->bssid[1], result->bssid[2],
           result->bssid[3], result->bssid[4], result->bssid[5], result->rssi, hasMeshConfig ? 1 : 0);
}

void HaLowInterface::onMeshScanComplete(enum mmwlan_scan_state scan_state)
{
    scanInProgress = false;
    LOG_INFO("HaLow: mesh info request complete state=%d seen=%u", (int)scan_state, meshPeerSeen ? 1 : 0);
    printf("HaLow: mesh info request complete state=%d seen=%u\n", (int)scan_state, meshPeerSeen ? 1 : 0);
}
#endif

void HaLowInterface::onFrameReceived(const uint8_t *payload, size_t payload_len, int8_t rssi, const uint8_t *srcMac, size_t srcMacLen)
{
    if (!payload || payload_len < sizeof(PacketHeader)) {
        LOG_WARN("HaLow: rx data too short len=%u", (unsigned)payload_len);
        printf("HaLow: rx data too short len=%u\n", (unsigned)payload_len);
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

    const PacketHeader *h = reinterpret_cast<const PacketHeader *>(payload);
#ifdef USE_MM_IOT_ESP32
    if (srcMacLen >= 6) {
        cachePeerMac(h->from, srcMac);
    }
#endif
    // Unpack the RadioBuffer into a MeshPacket (mirrors the LoRa RX decode).
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

    logHaLowMeshPacket("rx", payload, payload_len, p->rx_rssi, true);

    deliverToReceiver(p);
}

#endif // USE_HALOW_RADIO
