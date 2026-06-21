#include "ZephyrBluetooth.h"

#include "BluetoothCommon.h"
#include "BluetoothStatus.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "mesh/PhoneAPI.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(nrf54_ble, LOG_LEVEL_INF);

static struct bt_conn *currentConn;
static bool btReady;
static bool advertising;
static uint8_t fromRadioValue[meshtastic_FromRadio_size];
static uint16_t fromRadioValueLen;
static uint32_t fromNumValue;
static uint8_t batteryLevel = 100;

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
#define BT_UUID_FROMRADIO_VAL BT_UUID_128_ENCODE(0x2c55e69e, 0x499e, 0x11ed, 0xb878, 0x0242ac120002)
#define BT_UUID_FROMNUM_VAL BT_UUID_128_ENCODE(0xed9da18c, 0xa800, 0x4f66, 0xa670, 0xaa7547e34453)
#define BT_UUID_LOGRADIO_VAL BT_UUID_128_ENCODE(0x5a3d6e49, 0x06e6, 0x4423, 0x9944, 0xe9de8cdf9547)

static struct bt_uuid_128 meshSvcUuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_SERVICE_VAL);
static struct bt_uuid_128 toRadioUuid = BT_UUID_INIT_128(BT_UUID_TORADIO_VAL);
static struct bt_uuid_128 fromRadioUuid = BT_UUID_INIT_128(BT_UUID_FROMRADIO_VAL);
static struct bt_uuid_128 fromNumUuid = BT_UUID_INIT_128(BT_UUID_FROMNUM_VAL);
static struct bt_uuid_128 logRadioUuid = BT_UUID_INIT_128(BT_UUID_LOGRADIO_VAL);

static ssize_t readFromRadio(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)conn;
    const uint8_t *value = fromRadioValue;
    if (phoneApi) {
        fromRadioValueLen = phoneApi->getFromRadio(fromRadioValue);
    } else {
        fromRadioValueLen = 0;
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, fromRadioValueLen);
}

static ssize_t readFromNum(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)attr;
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
    if (phoneApi) {
        phoneApi->handleToRadio(static_cast<const uint8_t *>(buf), len);
    }
    return len;
}

static ssize_t readBattery(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    (void)attr;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &batteryLevel, sizeof(batteryLevel));
}

BT_GATT_SERVICE_DEFINE(meshtasticSvc, BT_GATT_PRIMARY_SERVICE(&meshSvcUuid),
                       BT_GATT_CHARACTERISTIC(&toRadioUuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, nullptr, writeToRadio,
                                              nullptr),
                       BT_GATT_CHARACTERISTIC(&fromRadioUuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, readFromRadio, nullptr,
                                              nullptr),
                       BT_GATT_CHARACTERISTIC(&fromNumUuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                                              readFromNum, nullptr, &fromNumValue),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                       BT_GATT_CHARACTERISTIC(&logRadioUuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                                              readLogRadio, nullptr, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

BT_GATT_SERVICE_DEFINE(batterySvc, BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
                       BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, readBattery, nullptr, &batteryLevel),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

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
    fromNumValue = fromRadioNum;
    bt_gatt_notify(nullptr, &meshtasticSvc.attrs[6], &fromNumValue, sizeof(fromNumValue));
}

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MESHTASTIC_SERVICE_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_WRN("BLE connect failed err=%u", err);
        return;
    }
    currentConn = bt_conn_ref(conn);
    if (!phoneApi) {
        phoneApi = new ZephyrBluetoothPhoneAPI();
    }
    LOG_INF("BLE connected");
    meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&status);
    powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (currentConn) {
        bt_conn_unref(currentConn);
        currentConn = nullptr;
    }
    if (phoneApi) {
        phoneApi->close();
    }
    LOG_INF("BLE disconnected reason=%u", reason);
    meshtastic::BluetoothStatus status(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&status);
    advertising = false;
    nrf54BluetoothSetEnabled(true);
}

BT_CONN_CB_DEFINE(connCallbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

void nrf54BluetoothSetEnabled(bool enable)
{
    if (!enable) {
        if (advertising) {
            bt_le_adv_stop();
            advertising = false;
        }
        return;
    }

    if (!config.bluetooth.enabled) {
        LOG_WRN("Bluetooth disabled in Meshtastic config; advertising anyway for nRF54 bring-up");
    }

    if (!btReady) {
        int err = bt_enable(nullptr);
        if (err && err != -EALREADY) {
            LOG_ERR("bt_enable returned err=%d", err);
            return;
        }
        btReady = true;
        LOG_INF("Bluetooth initialized");
    }

    if (!phoneApi) {
        phoneApi = new ZephyrBluetoothPhoneAPI();
    }

    if (!advertising && !currentConn) {
        const char *name = getDeviceName();
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
    bt_gatt_notify(nullptr, &batterySvc.attrs[2], &batteryLevel, sizeof(batteryLevel));
}
