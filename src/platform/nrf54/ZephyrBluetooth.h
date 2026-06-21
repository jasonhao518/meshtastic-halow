#pragma once

#include <stdint.h>

void nrf54BluetoothSetEnabled(bool enable);
void nrf54BluetoothStartAsync();
void nrf54BluetoothMarkAppReady();
bool nrf54BluetoothIsConnected();
int nrf54BluetoothGetRssi();
void nrf54BluetoothUpdateBatteryLevel(uint8_t level);
