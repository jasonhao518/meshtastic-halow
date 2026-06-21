#include "ZephyrBluetooth.h"

#include "BluetoothCommon.h"
#include "BluetoothStatus.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "mesh/mesh-pb-constants.h"
#include "mesh/PhoneAPI.h"
#include "target_specific.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>
#include <string>

LOG_MODULE_REGISTER(nrf54_ble, LOG_LEVEL_INF);

static struct bt_conn *currentConn;
static bool btReady;
static bool advertising;
static bool appReady;
static bool asyncStarted;
static bool enablePending;
static bool authRegistered;
static constexpr size_t bleThreadStackSize = 12288;
static constexpr unsigned int fixedPasskey = 123456;
static uint8_t fromRadioValue[meshtastic_FromRadio_size];
static uint16_t fromRadioValueLen;
static uint32_t fromRadioReadGeneration;
static bool deferSteadyReadAfterQueueStatus;
static uint32_t fromNumValue;
static uint8_t batteryLevel = 100;
static char advertisedName[20];

class ZephyrBluetoothPhoneAPI : public PhoneAPI
{
  public:
    ZephyrBluetoothPhoneAPI();

    bool checkIsConnected() override;
    void onConnectionChanged(bool connected) override;
    void onNowHasData(uint32_t fromRadioNum) override;
};

static ZephyrBluetoothPhoneAPI *phoneApi;

#define BT_UUID_MESHTASTIC_SERVICE_VAL BT_UUID_128_ENCODE(0x6ba1b218, 0x15a8, 0x461f, 0x9fa8, 0x5dcae273eafd)
#define BT_UUID_TORADIO_VAL BT_UUID_128_ENCODE(0xf75c76d2, 0x129e, 0x4dad, 0xa1dd, 0x7866124401e7)
#define BT_UUID_FROMRADIO_VAL BT_UUID_128_ENCODE(0x2c55e69e, 0x4993, 0x11ed, 0xb878, 0x0242ac120002)
#define BT_UUID_FROMNUM_VAL BT_UUID_128_ENCODE(0xed9da18c, 0xa800, 0x4f66, 0xa670, 0xaa7547e34453)
#define BT_UUID_LOGRADIO_VAL BT_UUID_128_ENCODE(0x5a3d6e49, 0x06e6, 0x4423, 0x9944, 0xe9de8cdf9547)

static struct bt_uuid_128 meshSvcUuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_SERVICE_VAL);
static struct bt_uuid_128 toRadioUuid = BT_UUID_INIT_128(BT_UUID_TORADIO_VAL);
static struct bt_uuid_128 fromRadioUuid = BT_UUID_INIT_128(BT_UUID_FROMRADIO_VAL);
static struct bt_uuid_128 fromNumUuid = BT_UUID_INIT_128(BT_UUID_FROMNUM_VAL);
static struct bt_uuid_128 logRadioUuid = BT_UUID_INIT_128(BT_UUID_LOGRADIO_VAL);

static constexpr bt_gatt_perm meshReadPerm = static_cast<bt_gatt_perm>(BT_GATT_PERM_READ | BT_GATT_PERM_READ_ENCRYPT);
static constexpr bt_gatt_perm meshWritePerm = static_cast<bt_gatt_perm>(BT_GATT_PERM_WRITE | BT_GATT_PERM_WRITE_ENCRYPT);
static constexpr bt_gatt_perm meshCccPerm = static_cast<bt_gatt_perm>(meshReadPerm | meshWritePerm);

static void drainKickWorkHandler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(drainKickWork, drainKickWorkHandler);

static bool isAckedQueueStatusFrame(const uint8_t *buf, uint16_t len)
{
    meshtastic_FromRadio fromRadio = meshtastic_FromRadio_init_zero;
    return pb_decode_from_bytes(buf, len, &meshtastic_FromRadio_msg, &fromRadio) &&
           fromRadio.which_payload_variant == meshtastic_FromRadio_queueStatus_tag &&
           fromRadio.queueStatus.mesh_packet_id != 0;
}

static ssize_t readFromRadio(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)conn;
    const uint8_t *value = fromRadioValue;
    if (offset == 0 && appReady && phoneApi) {
        if (deferSteadyReadAfterQueueStatus && phoneApi->isSendingPackets()) {
            deferSteadyReadAfterQueueStatus = false;
            fromRadioValueLen = 0;
            k_work_reschedule(&drainKickWork, K_MSEC(30));
            LOG_INF("BLE FromRadio inserting empty read after queue status");
        } else {
            fromRadioValueLen = phoneApi->getFromRadio(fromRadioValue);
            if (phoneApi->isSendingPackets() && fromRadioValueLen != 0 && isAckedQueueStatusFrame(fromRadioValue, fromRadioValueLen)) {
                deferSteadyReadAfterQueueStatus = true;
                k_work_reschedule(&drainKickWork, K_MSEC(30));
                LOG_INF("BLE FromRadio queue status boundary armed gen=%u", fromRadioReadGeneration + 1);
            }
        }
        fromRadioReadGeneration++;
    } else if (offset == 0) {
        fromRadioValueLen = 0;
        fromRadioReadGeneration++;
    }

    if (offset > fromRadioValueLen) {
        LOG_WRN("BLE FromRadio read invalid offset=%u cachedLen=%u gen=%u", offset, fromRadioValueLen,
                fromRadioReadGeneration);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    ssize_t readLen = bt_gatt_attr_read(conn, attr, buf, len, offset, value, fromRadioValueLen);
    LOG_INF("BLE FromRadio read offset=%u mtuLen=%u cachedLen=%u gen=%u return=%d", offset, len, fromRadioValueLen,
            fromRadioReadGeneration, static_cast<int>(readLen));
    return readLen;
}

static void resetFromRadioReadCache()
{
    fromRadioValueLen = 0;
    fromRadioReadGeneration++;
    deferSteadyReadAfterQueueStatus = false;
}

static ssize_t readFromNum(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)attr;
    LOG_INF("BLE FromNum read offset=%u value=%u", offset, fromNumValue);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &fromNumValue, sizeof(fromNumValue));
}

static ssize_t readLogRadio(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    static const uint8_t empty = 0;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &empty, 0);
}

static ssize_t writeToRadio(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset,
                            uint8_t flags)
{
    (void)conn;
    (void)attr;
    (void)flags;
    if (offset != 0 || len > MAX_TO_FROM_RADIO_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    LOG_INF("BLE ToRadio write len=%u appReady=%u phoneApi=%p", len, appReady, phoneApi);
    if (!appReady) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    resetFromRadioReadCache();
    if (phoneApi) {
        bool handled = phoneApi->handleToRadio(static_cast<const uint8_t *>(buf), len);
        LOG_INF("BLE ToRadio handled=%u", handled);
    }
    return len;
}

static void fromNumCccChanged(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr;
    LOG_INF("BLE FromNum CCC value=0x%04x notify=%u", value, value == BT_GATT_CCC_NOTIFY);
}

static void logRadioCccChanged(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr;
    LOG_INF("BLE LogRadio CCC value=0x%04x notify=%u indicate=%u", value, value == BT_GATT_CCC_NOTIFY,
            value == BT_GATT_CCC_INDICATE);
}

static ssize_t readBattery(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)attr;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &batteryLevel, sizeof(batteryLevel));
}

BT_GATT_SERVICE_DEFINE(meshtasticSvc, BT_GATT_PRIMARY_SERVICE(&meshSvcUuid),
                       BT_GATT_CHARACTERISTIC(&toRadioUuid.uuid, BT_GATT_CHRC_WRITE, meshWritePerm, nullptr, writeToRadio, nullptr),
                       BT_GATT_CHARACTERISTIC(&fromRadioUuid.uuid, BT_GATT_CHRC_READ, meshReadPerm, readFromRadio, nullptr, nullptr),
                       BT_GATT_CHARACTERISTIC(&fromNumUuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, meshReadPerm,
                                              readFromNum, nullptr, &fromNumValue),
                       BT_GATT_CCC(fromNumCccChanged, meshCccPerm),
                       BT_GATT_CHARACTERISTIC(&logRadioUuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, meshReadPerm,
                                              readLogRadio, nullptr, nullptr),
                       BT_GATT_CCC(logRadioCccChanged, meshCccPerm));

BT_GATT_SERVICE_DEFINE(batterySvc, BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
                       BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, readBattery, nullptr, &batteryLevel),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static void drainKickWorkHandler(struct k_work *work)
{
    (void)work;
    if (!currentConn || !appReady || !phoneApi || !phoneApi->isSendingPackets()) {
        LOG_INF("BLE FromRadio delayed kick skipped connected=%u appReady=%u phoneApi=%p", currentConn != nullptr, appReady,
                phoneApi);
        return;
    }

    fromNumValue++;
    int err = bt_gatt_notify(nullptr, &meshtasticSvc.attrs[6], &fromNumValue, sizeof(fromNumValue));
    LOG_INF("BLE FromNum delayed kick value=%u err=%d", fromNumValue, err);
}

ZephyrBluetoothPhoneAPI::ZephyrBluetoothPhoneAPI()
{
    api_type = TYPE_BLE;
}

bool ZephyrBluetoothPhoneAPI::checkIsConnected()
{
    return currentConn != nullptr;
}

void ZephyrBluetoothPhoneAPI::onConnectionChanged(bool connected)
{
    meshtastic::BluetoothStatus status(connected ? meshtastic::BluetoothStatus::ConnectionState::CONNECTED
                                                 : meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&status);
}

void ZephyrBluetoothPhoneAPI::onNowHasData(uint32_t fromRadioNum)
{
    PhoneAPI::onNowHasData(fromRadioNum);
    fromNumValue = getFromRadioNotifyNum(fromRadioNum);
    int err = bt_gatt_notify(nullptr, &meshtasticSvc.attrs[6], &fromNumValue, sizeof(fromNumValue));
    LOG_INF("BLE FromNum notify requested=%u value=%u err=%d", fromRadioNum, fromNumValue, err);
}

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MESHTASTIC_SERVICE_VAL),
};

static void restartAdvertisingWorkHandler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(restartAdvertisingWork, restartAdvertisingWorkHandler);

static const char *getAdvertisedName()
{
    if (appReady) {
        const char *name = getDeviceName();
        if (name && name[0] && name[0] != '_') {
            return name;
        }
    }

    uint8_t dmac[6];
    getMacAddr(dmac);
    snprintf(advertisedName, sizeof(advertisedName), "Meshtastic_%02x%02x", dmac[4], dmac[5]);
    return advertisedName;
}

static void startAdvertising()
{
    if (!btReady || advertising || currentConn) {
        LOG_INF("BLE advertise skipped ready=%u advertising=%u connected=%u", btReady, advertising, currentConn != nullptr);
        return;
    }

    const char *name = getAdvertisedName();
    int nameErr = bt_set_name(name);
    if (nameErr) {
        LOG_WRN("BLE device name set failed err=%d name=%s", nameErr, name);
    }
    struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, name, static_cast<uint8_t>(strlen(name))),
    };
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("BLE advertising start failed err=%d", err);
        return;
    }
    advertising = true;
    LOG_INF("BLE advertising as %s", name);
}

static void scheduleAdvertisingRestart(k_timeout_t delay)
{
    if (!btReady || currentConn || !appReady) {
        LOG_INF("BLE advertise restart skipped ready=%u connected=%u appReady=%u", btReady, currentConn != nullptr, appReady);
        return;
    }

    advertising = false;
    int err = k_work_reschedule(&restartAdvertisingWork, delay);
    LOG_INF("BLE advertising restart scheduled err=%d", err);
}

static void restartAdvertisingWorkHandler(struct k_work *work)
{
    (void)work;
    startAdvertising();
}

static void pairingConfirm(struct bt_conn *conn)
{
    LOG_INF("BLE pairing confirm");
    bt_conn_auth_pairing_confirm(conn);
}

static void passkeyDisplay(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("BLE pairing passkey %06u", passkey);
    powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
    meshtastic::BluetoothStatus status(std::to_string(passkey));
    bluetoothStatus->updateStatus(&status);
}

static void authCancel(struct bt_conn *conn)
{
    LOG_WRN("BLE pairing cancelled");
}

static void pairingComplete(struct bt_conn *conn, bool bonded)
{
    LOG_INF("BLE pairing complete bonded=%u", bonded);
    meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&status);
}

static void pairingFailed(struct bt_conn *conn, enum bt_security_err reason)
{
    LOG_WRN("BLE pairing failed reason=%u", reason);
    meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&status);
}

static struct bt_conn_auth_cb authCallbacks;
static struct bt_conn_auth_info_cb authInfoCallbacks;

static void configureSecurity()
{
    if (authRegistered) {
        return;
    }

    authCallbacks.passkey_display = passkeyDisplay;
    authCallbacks.cancel = authCancel;
    authCallbacks.pairing_confirm = pairingConfirm;
    authInfoCallbacks.pairing_complete = pairingComplete;
    authInfoCallbacks.pairing_failed = pairingFailed;

    int err = bt_conn_auth_cb_register(&authCallbacks);
    if (err) {
        LOG_WRN("BLE auth callback register failed err=%d", err);
    }
    err = bt_conn_auth_info_cb_register(&authInfoCallbacks);
    if (err) {
        LOG_WRN("BLE auth info callback register failed err=%d", err);
    }
    err = bt_passkey_set(fixedPasskey);
    if (err) {
        LOG_WRN("BLE fixed passkey set failed err=%d", err);
    } else {
        LOG_INF("BLE fixed passkey set to %06u", fixedPasskey);
    }
    authRegistered = true;
}

static void btReadyCallback(int err)
{
    enablePending = false;
    if (err) {
        LOG_ERR("bt_enable callback err=%d", err);
        return;
    }

    btReady = true;
    LOG_INF("Bluetooth initialized");
    configureSecurity();
    startAdvertising();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_WRN("BLE connect failed err=%u", err);
        scheduleAdvertisingRestart(K_MSEC(500));
        return;
    }
    currentConn = bt_conn_ref(conn);
    advertising = false;
    struct bt_conn_info info;
    int infoErr = bt_conn_get_info(conn, &info);
    if (!infoErr && info.type == BT_CONN_TYPE_LE) {
        LOG_INF("BLE connected appReady=%u interval=%u latency=%u timeout=%u", appReady, info.le.interval, info.le.latency,
                info.le.timeout);
    } else {
        LOG_INF("BLE connected appReady=%u infoErr=%d", appReady, infoErr);
    }
    if (appReady && !phoneApi) {
        phoneApi = new ZephyrBluetoothPhoneAPI();
    }
    if (appReady) {
        meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&status);
        powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
        int secErr = bt_conn_set_security(conn, BT_SECURITY_L3);
        LOG_INF("BLE security requested err=%d", secErr);
    }
}

static void securityChanged(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    (void)conn;
    if (err) {
        LOG_WRN("BLE security changed failed level=%u err=%u", level, err);
    } else {
        LOG_INF("BLE security changed level=%u", level);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (currentConn) {
        bt_conn_unref(currentConn);
        currentConn = nullptr;
    }
    if (appReady && phoneApi) {
        phoneApi->close();
    }
    resetFromRadioReadCache();
    LOG_INF("BLE disconnected reason=%u appReady=%u", reason, appReady);
    if (appReady) {
        meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        bluetoothStatus->updateStatus(&status);
    }
    scheduleAdvertisingRestart(K_MSEC(500));
}

BT_CONN_CB_DEFINE(connCallbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = securityChanged,
};

static void bleThreadEntry(void *, void *, void *)
{
    LOG_INF("BLE worker starting init stack=%u", bleThreadStackSize);
    nrf54BluetoothSetEnabled(true);
    LOG_INF("BLE worker init returned");
}

K_THREAD_STACK_DEFINE(bleThreadStack, bleThreadStackSize);
static struct k_thread bleThread;

void nrf54BluetoothStartAsync()
{
    if (asyncStarted) {
        LOG_INF("BLE async start already requested");
        return;
    }

    asyncStarted = true;
    k_thread_create(&bleThread, bleThreadStack, K_THREAD_STACK_SIZEOF(bleThreadStack), bleThreadEntry, nullptr, nullptr, nullptr,
                    K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(&bleThread, "nrf54_ble");
    LOG_INF("BLE async start requested prio=%d", K_PRIO_PREEMPT(8));
}

void nrf54BluetoothSetEnabled(bool enable)
{
    LOG_INF("BLE set enabled=%u ready=%u advertising=%u connected=%u appReady=%u", enable, btReady, advertising,
            currentConn != nullptr, appReady);
    if (!enable) {
        if (advertising) {
            int err = bt_le_adv_stop();
            LOG_INF("BLE advertising stop err=%d", err);
            advertising = false;
        }
        return;
    }

    if (appReady && !config.bluetooth.enabled) {
        LOG_WRN("Bluetooth disabled in Meshtastic config; enabling for nRF54 bring-up");
        config.bluetooth.enabled = true;
    }

    if (!btReady) {
        if (enablePending) {
            LOG_INF("BLE enable already pending");
            return;
        }

        enablePending = true;
        LOG_INF("Calling bt_enable");
        int err = bt_enable(btReadyCallback);
        LOG_INF("bt_enable returned err=%d", err);
        if (err && err != -EALREADY) {
            LOG_ERR("bt_enable returned err=%d", err);
            enablePending = false;
            return;
        }
        if (err == -EALREADY) {
            enablePending = false;
            btReady = true;
            LOG_INF("Bluetooth already initialized");
            startAdvertising();
        } else {
            LOG_INF("Bluetooth enable requested");
        }
        return;
    }

    if (appReady && !phoneApi) {
        phoneApi = new ZephyrBluetoothPhoneAPI();
    }

    startAdvertising();
}

void nrf54BluetoothMarkAppReady()
{
    if (appReady) {
        LOG_INF("BLE app already ready; connected=%u advertising=%u", currentConn != nullptr, advertising);
        return;
    }

    appReady = true;
    LOG_INF("BLE app ready; connected=%u advertising=%u", currentConn != nullptr, advertising);
    if (!phoneApi) {
        phoneApi = new ZephyrBluetoothPhoneAPI();
    }
    if (!asyncStarted) {
        LOG_INF("BLE app ready; starting worker");
        nrf54BluetoothStartAsync();
        return;
    }
    if (currentConn) {
        meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&status);
        powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
    } else {
        if (advertising) {
            int err = bt_le_adv_stop();
            LOG_INF("BLE advertising restart for Meshtastic name err=%d", err);
            advertising = false;
        }
        nrf54BluetoothSetEnabled(true);
    }
}

bool nrf54BluetoothIsAppReady()
{
    return appReady;
}

bool nrf54BluetoothIsConnected()
{
    return currentConn != nullptr;
}

int nrf54BluetoothGetRssi()
{
    return 0;
}

void nrf54BluetoothUpdateBatteryLevel(uint8_t level)
{
    batteryLevel = level;
    if (btReady) {
        bt_gatt_notify(nullptr, &batterySvc.attrs[2], &batteryLevel, sizeof(batteryLevel));
    }
}
