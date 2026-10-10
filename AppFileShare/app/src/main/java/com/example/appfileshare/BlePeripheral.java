package com.example.appfileshare;

import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.content.Context;
import android.os.Build;
import android.os.ParcelUuid;

import java.nio.charset.StandardCharsets;
import java.util.UUID;

public final class BlePeripheral {
    public static final UUID SERVICE_UUID = UUID.fromString("6b8a1f40-2c7d-4e15-9a33-0d5e7c1b4a86");
    public static final UUID RX_UUID = UUID.fromString("6b8a1f41-2c7d-4e15-9a33-0d5e7c1b4a86");
    public static final UUID TX_UUID = UUID.fromString("6b8a1f42-2c7d-4e15-9a33-0d5e7c1b4a86");
    public static final UUID CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    public interface Callback {
        void onStatus(String message);
        void onReady(BluetoothDevice device, int attPayload);
        void onDisconnected(String reason);
        void onPacket(byte[] packet);
    }

    private final Context context;
    private final Callback callback;
    private BluetoothGattServer server;
    private BluetoothLeAdvertiser advertiser;
    private BluetoothGattCharacteristic tx;
    private BluetoothDevice connected;
    private int attPayload = 20;
    private boolean advertising;

    public BlePeripheral(Context context, Callback callback) {
        this.context = context.getApplicationContext();
        this.callback = callback;
    }

    public boolean isAdvertising() {
        return advertising;
    }

    public BluetoothDevice connectedDevice() {
        return connected;
    }

    public void start(String deviceId) {
        BluetoothManager manager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null || manager.getAdapter() == null) {
            callback.onStatus("这台手机没有蓝牙适配器");
            return;
        }
        if (!manager.getAdapter().isEnabled()) {
            callback.onStatus("蓝牙未开启");
            return;
        }
        advertiser = manager.getAdapter().getBluetoothLeAdvertiser();
        if (advertiser == null) {
            callback.onStatus("这台手机不支持低功耗蓝牙广播");
            return;
        }
        server = manager.openGattServer(context, gattCallback);
        if (server == null) {
            callback.onStatus("无法创建 GATT Server");
            return;
        }
        BluetoothGattService service = new BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY);
        BluetoothGattCharacteristic rx = new BluetoothGattCharacteristic(
                RX_UUID,
                BluetoothGattCharacteristic.PROPERTY_WRITE | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattCharacteristic.PERMISSION_WRITE);
        tx = new BluetoothGattCharacteristic(
                TX_UUID,
                BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattCharacteristic.PERMISSION_READ);
        BluetoothGattDescriptor cccd = new BluetoothGattDescriptor(
                CCCD_UUID,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        tx.addDescriptor(cccd);
        service.addCharacteristic(rx);
        service.addCharacteristic(tx);
        server.addService(service);

        AdvertiseSettings settings = new AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setConnectable(true)
                .setTimeout(0)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
                .build();
        AdvertiseData data = new AdvertiseData.Builder()
                .setIncludeDeviceName(false)
                .addServiceUuid(new ParcelUuid(SERVICE_UUID))
                .build();
        byte[] token = deviceId.length() > 4
                ? deviceId.substring(4, Math.min(deviceId.length(), 12)).getBytes(StandardCharsets.UTF_8)
                : deviceId.getBytes(StandardCharsets.UTF_8);
        AdvertiseData scanResponse = new AdvertiseData.Builder()
                .addServiceData(new ParcelUuid(SERVICE_UUID), token)
                .build();
        advertiser.startAdvertising(settings, data, scanResponse, advertiseCallback);
    }

    public void stop() {
        advertising = false;
        if (advertiser != null) {
            try {
                advertiser.stopAdvertising(advertiseCallback);
            } catch (SecurityException ignored) {
            }
            advertiser = null;
        }
        if (connected != null && server != null) {
            try {
                server.cancelConnection(connected);
            } catch (SecurityException ignored) {
            }
        }
        connected = null;
        if (server != null) {
            server.close();
            server = null;
        }
        callback.onStatus("已停止设备发现");
    }

    public void disconnect() {
        if (connected != null && server != null) {
            try {
                server.cancelConnection(connected);
            } catch (SecurityException ex) {
                callback.onStatus("断开连接失败：" + ex.getMessage());
            }
        }
    }

    public void send(byte[] packet) {
        if (server == null || connected == null || tx == null) {
            callback.onStatus("通知通道还不可用，无法发送蓝牙数据");
            return;
        }
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                server.notifyCharacteristicChanged(connected, tx, false, packet);
            } else {
                tx.setValue(packet);
                server.notifyCharacteristicChanged(connected, tx, false);
            }
        } catch (SecurityException ex) {
            callback.onStatus("发送蓝牙通知失败：" + ex.getMessage());
        }
    }

    private final AdvertiseCallback advertiseCallback = new AdvertiseCallback() {
        @Override
        public void onStartSuccess(AdvertiseSettings settingsInEffect) {
            advertising = true;
            callback.onStatus("正在等待 Windows 电脑连接");
        }

        @Override
        public void onStartFailure(int errorCode) {
            advertising = false;
            callback.onStatus("开启蓝牙广播失败，代码 " + errorCode);
        }
    };

    private final BluetoothGattServerCallback gattCallback = new BluetoothGattServerCallback() {
        @Override
        public void onConnectionStateChange(BluetoothDevice device, int status, int newState) {
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                connected = device;
                callback.onStatus("Windows 已连上蓝牙，等待通知订阅");
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                if (connected != null && connected.getAddress().equals(device.getAddress())) {
                    connected = null;
                    callback.onDisconnected(status == BluetoothGatt.GATT_SUCCESS ? "蓝牙连接已断开" : "蓝牙连接中断");
                }
            }
        }

        @Override
        public void onMtuChanged(BluetoothDevice device, int mtu) {
            attPayload = Math.max(20, mtu - 3);
        }

        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice device, int requestId,
                                                 BluetoothGattCharacteristic characteristic, boolean preparedWrite,
                                                 boolean responseNeeded, int offset, byte[] value) {
            if (responseNeeded && server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value);
            }
            if (RX_UUID.equals(characteristic.getUuid()) && offset == 0 && value != null) {
                callback.onPacket(value);
            }
        }

        @Override
        public void onDescriptorWriteRequest(BluetoothDevice device, int requestId, BluetoothGattDescriptor descriptor,
                                             boolean preparedWrite, boolean responseNeeded, int offset, byte[] value) {
            if (responseNeeded && server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value);
            }
            if (CCCD_UUID.equals(descriptor.getUuid()) && value != null && value.length > 0 && (value[0] & 0x01) != 0) {
                connected = device;
                callback.onReady(device, attPayload);
            }
        }
    };
}
