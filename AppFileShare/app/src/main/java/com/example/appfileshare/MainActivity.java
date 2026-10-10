package com.example.appfileshare;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.activity.EdgeToEdge;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

public class MainActivity extends AppCompatActivity implements NativeLink.Listener, BlePeripheral.Callback {
    private static final int P2P_CONNECTED = 6;
    private static final int ICE_FAILED = 7;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private TextView localNameText;
    private TextView deviceIdText;
    private TextView advertiseText;
    private TextView bleText;
    private TextView p2pText;
    private TextView peerText;
    private TextView transferText;
    private TextView speedText;
    private TextView logText;
    private ScrollView logScroll;
    private ProgressBar progressBar;
    private Button sendButton;
    private BlePeripheral peripheral;
    private NativeLink nativeLink;
    private String deviceId;
    private boolean p2pReady;
    private boolean awaitingDecision;
    private long progressBytes;
    private long progressMs;
    private final StringBuilder logBuilder = new StringBuilder();

    private final ActivityResultLauncher<String[]> permissionLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestMultiplePermissions(), result -> {
                boolean granted = Boolean.TRUE.equals(result.get(android.Manifest.permission.BLUETOOTH_ADVERTISE))
                        && Boolean.TRUE.equals(result.get(android.Manifest.permission.BLUETOOTH_CONNECT));
                if (!granted) {
                    setAdvertise("没有蓝牙广播或连接权限");
                    return;
                }
                ensureBluetoothAndAdvertise();
            });

    private final ActivityResultLauncher<Intent> openDocument =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() != RESULT_OK || result.getData() == null) return;
                queueUri(result.getData().getData());
            });

    private final ActivityResultLauncher<Intent> createDocument =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (!awaitingDecision) return;
                awaitingDecision = false;
                if (result.getResultCode() != RESULT_OK || result.getData() == null || result.getData().getData() == null) {
                    if (nativeLink != null) nativeLink.reject("用户取消保存");
                    return;
                }
                acceptUri(result.getData().getData());
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        EdgeToEdge.enable(this);
        setContentView(R.layout.activity_main);
        localNameText = findViewById(R.id.localNameText);
        deviceIdText = findViewById(R.id.deviceIdText);
        advertiseText = findViewById(R.id.advertiseText);
        bleText = findViewById(R.id.bleText);
        p2pText = findViewById(R.id.p2pText);
        peerText = findViewById(R.id.peerText);
        transferText = findViewById(R.id.transferText);
        speedText = findViewById(R.id.speedText);
        logText = findViewById(R.id.logText);
        logScroll = findViewById(R.id.logScroll);
        progressBar = findViewById(R.id.progressBar);
        sendButton = findViewById(R.id.sendButton);
        sendButton.setEnabled(false);

        deviceId = DeviceIdentity.id(this);
        String localName = localBluetoothName();
        localNameText.setText("本机名称：" + localName);
        deviceIdText.setText("本机应用设备 ID：" + deviceId);
        peripheral = new BlePeripheral(this, this);
        try {
            nativeLink = new NativeLink(this, deviceId, localName);
        } catch (Throwable error) {
            appendLog("原生库加载失败：" + error.getMessage());
        }

        findViewById(R.id.startButton).setOnClickListener(v -> requestAndAdvertise());
        findViewById(R.id.stopButton).setOnClickListener(v -> {
            peripheral.stop();
            if (nativeLink != null) nativeLink.transportDown("已停止发现");
        });
        findViewById(R.id.disconnectButton).setOnClickListener(v -> {
            peripheral.disconnect();
            if (nativeLink != null) nativeLink.transportDown("用户断开连接");
        });
        sendButton.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            openDocument.launch(intent);
        });
        findViewById(R.id.cancelSendButton).setOnClickListener(v -> {
            if (nativeLink != null) nativeLink.cancelSend();
        });
        findViewById(R.id.cancelReceiveButton).setOnClickListener(v -> {
            if (nativeLink != null) nativeLink.cancelReceive();
        });
    }

    @Override
    protected void onDestroy() {
        if (peripheral != null) peripheral.stop();
        if (nativeLink != null) nativeLink.close();
        super.onDestroy();
    }

    private void requestAndAdvertise() {
        permissionLauncher.launch(new String[]{
                android.Manifest.permission.BLUETOOTH_ADVERTISE,
                android.Manifest.permission.BLUETOOTH_CONNECT
        });
    }

    private void ensureBluetoothAndAdvertise() {
        BluetoothManager manager = (BluetoothManager) getSystemService(BLUETOOTH_SERVICE);
        if (manager == null || manager.getAdapter() == null) {
            setAdvertise("这台手机没有蓝牙适配器");
            return;
        }
        if (!manager.getAdapter().isEnabled()) {
            setAdvertise("蓝牙未开启");
            startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            return;
        }
        peripheral.start(deviceId);
    }

    private String localBluetoothName() {
        try {
            BluetoothManager manager = (BluetoothManager) getSystemService(BLUETOOTH_SERVICE);
            if (manager != null && manager.getAdapter() != null && manager.getAdapter().getName() != null) {
                return manager.getAdapter().getName();
            }
        } catch (SecurityException ignored) {
        }
        return android.os.Build.MODEL;
    }

    private void queueUri(Uri uri) {
        if (uri == null || nativeLink == null) return;
        try {
            String name = queryName(uri);
            long size = querySize(uri);
            if (size < 0) {
                appendLog("无法获取文件大小，已取消发送");
                return;
            }
            InputStream input = getContentResolver().openInputStream(uri);
            if (input == null) {
                appendLog("无法打开所选文件");
                return;
            }
            final long[] position = new long[]{0};
            String error = nativeLink.queueFile(name, size, (offset, length) -> {
                if (offset != position[0]) throw new IOException("文件读取位置不连续");
                byte[] buffer = new byte[length];
                int got = 0;
                while (got < length) {
                    int n = input.read(buffer, got, length - got);
                    if (n < 0) break;
                    got += n;
                }
                position[0] += got;
                if (got == buffer.length) return buffer;
                byte[] exact = new byte[got];
                System.arraycopy(buffer, 0, exact, 0, got);
                return exact;
            });
            if (error != null) appendLog(error);
            else if (!p2pReady) appendLog("文件已加入队列。P2P 尚未连接，连接成功后才会发送。");
        } catch (Exception ex) {
            appendLog("选择文件失败：" + ex.getMessage());
        }
    }

    private void acceptUri(Uri uri) {
        try {
            OutputStream output = getContentResolver().openOutputStream(uri);
            if (output == null) {
                nativeLink.reject("无法创建接收文件");
                return;
            }
            nativeLink.accept(new NativeLink.Writer() {
                @Override
                public void write(byte[] data) throws IOException {
                    output.write(data);
                }

                @Override
                public void discard() {
                    try {
                        output.close();
                    } catch (IOException ignored) {
                    }
                    try {
                        getContentResolver().delete(uri, null, null);
                    } catch (Exception ignored) {
                    }
                }
            });
        } catch (Exception ex) {
            nativeLink.reject("创建接收文件失败：" + ex.getMessage());
        }
    }

    private String queryName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        }
        return "file";
    }

    private long querySize(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (index >= 0 && !cursor.isNull(index)) return cursor.getLong(index);
            }
        }
        return -1;
    }

    private void setAdvertise(String text) {
        advertiseText.setText("广播：" + text);
        appendLog(text);
    }

    private void appendLog(String line) {
        if (logBuilder.length() > 6000) logBuilder.delete(0, 2000);
        logBuilder.append(line).append('\n');
        logText.setText(logBuilder.toString());
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void post(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) action.run();
        else mainHandler.post(action);
    }

    @Override
    public void onStatus(String message) {
        post(() -> setAdvertise(message));
    }

    @Override
    public void onReady(BluetoothDevice device, int attPayload) {
        post(() -> {
            peerText.setText("当前连接的 Windows 设备：" + safeName(device));
            bleText.setText("蓝牙：已连接");
            if (nativeLink != null) nativeLink.transportUp(attPayload);
        });
    }

    @Override
    public void onDisconnected(String reason) {
        post(() -> {
            bleText.setText("蓝牙：已断开");
            p2pText.setText("P2P：未连接");
            p2pReady = false;
            sendButton.setEnabled(false);
            peerText.setText("当前没有连接 Windows 电脑");
            if (nativeLink != null) nativeLink.transportDown(reason);
            appendLog(reason);
        });
    }

    @Override
    public void onPacket(byte[] packet) {
        if (nativeLink != null) nativeLink.onPacket(packet);
    }

    @Override
    public void onSendBle(byte[] packet) {
        post(() -> peripheral.send(packet));
    }

    @Override
    public void onState(int state, String detail) {
        post(() -> {
            p2pReady = state == P2P_CONNECTED;
            sendButton.setEnabled(p2pReady);
            if (state == P2P_CONNECTED) {
                bleText.setText("蓝牙：已连接");
                p2pText.setText("P2P：已连接");
            } else if (state == 4 || state == 5) {
                bleText.setText("蓝牙：已连接");
                p2pText.setText("P2P：正在连接");
            } else if (state == ICE_FAILED) {
                bleText.setText("蓝牙：已连接");
                p2pText.setText("P2P：直连失败");
            } else if (state == 3) {
                bleText.setText("蓝牙：已连接");
                p2pText.setText("P2P：未连接");
            } else if (state == 9 || state == 0) {
                if (state == 9) {
                    bleText.setText("蓝牙：已断开");
                    p2pText.setText("P2P：未连接");
                }
            }
            if (detail != null && !detail.isEmpty()) appendLog(detail);
        });
    }

    @Override
    public void onLog(String line) {
        post(() -> appendLog(line));
    }

    @Override
    public void onOffer(String name, long size) {
        post(() -> {
            awaitingDecision = true;
            new AlertDialog.Builder(this)
                    .setTitle("接收文件")
                    .setMessage("电脑正在向手机发送文件\n发送方：" + peerText.getText()
                            + "\n文件名称：" + name + "\n文件大小：" + formatBytes(size))
                    .setPositiveButton("接收", (dialog, which) -> {
                        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                        intent.addCategory(Intent.CATEGORY_OPENABLE);
                        intent.setType("*/*");
                        intent.putExtra(Intent.EXTRA_TITLE, name);
                        createDocument.launch(intent);
                    })
                    .setNegativeButton("拒绝", (dialog, which) -> {
                        awaitingDecision = false;
                        if (nativeLink != null) nativeLink.reject("用户拒绝接收");
                    })
                    .setOnCancelListener(dialog -> {
                        awaitingDecision = false;
                        if (nativeLink != null) nativeLink.reject("用户拒绝接收");
                    })
                    .show();
        });
    }

    @Override
    public void onProgress(boolean sending, String name, long done, long total) {
        post(() -> {
            long now = android.os.SystemClock.elapsedRealtime();
            String speed = "速度计算中";
            if (progressMs > 0 && now > progressMs && done >= progressBytes) {
                double dt = (now - progressMs) / 1000.0;
                if (dt > 0.05) {
                    double bps = (done - progressBytes) / dt;
                    speed = String.format(Locale.US, "%s/s", formatBytes((long) bps));
                    if (bps > 1 && total > done) {
                        speed += String.format(Locale.US, "，剩余约 %d 秒", (long) ((total - done) / bps));
                    }
                }
            }
            progressBytes = done;
            progressMs = now;
            transferText.setText((sending ? "正在发送 " : "正在接收 ") + name + "  " + formatBytes(done) + " / " + formatBytes(total));
            speedText.setText(speed);
            progressBar.setProgress(total == 0 ? 100 : (int) ((done * 100) / total));
        });
    }

    @Override
    public void onFinished(boolean sending, boolean ok, String message) {
        post(() -> {
            transferText.setText(message);
            if (ok) progressBar.setProgress(100);
            appendLog(message);
        });
    }

    @Override
    public void onPeerDecision(boolean accepted, String reason) {
        post(() -> transferText.setText(accepted ? "对方已接受，开始发送" : "对方未接受：" + reason));
    }

    @Override
    public void onPeer(String name, String id) {
        post(() -> peerText.setText("当前连接的 Windows 设备：" + name + " / " + id));
    }

    private String safeName(BluetoothDevice device) {
        try {
            String name = device.getName();
            if (name != null && !name.isEmpty()) return name;
        } catch (SecurityException ignored) {
        }
        return device.getAddress();
    }

    private String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(Locale.US, "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(Locale.US, "%.1f MB", bytes / 1024.0 / 1024.0);
        return String.format(Locale.US, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
    }
}
