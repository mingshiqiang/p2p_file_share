package com.example.appfileshare;

import java.io.IOException;

public final class NativeLink {
    static {
        System.loadLibrary("fileshare_native");
    }

    public interface Listener {
        void onSendBle(byte[] packet);
        void onState(int state, String detail);
        void onLog(String line);
        void onOffer(String name, long size);
        void onProgress(boolean sending, String name, long done, long total);
        void onFinished(boolean sending, boolean ok, String message);
        void onPeerDecision(boolean accepted, String reason);
        void onPeer(String name, String id);
    }

    public interface Reader {
        byte[] read(long offset, int length) throws IOException;
    }

    public interface Writer {
        void write(byte[] data) throws IOException;
        void discard();
    }

    private long handle;

    public NativeLink(Listener listener, String deviceId, String deviceName) {
        handle = nativeCreate(listener, deviceId, deviceName);
        if (handle == 0) {
            throw new IllegalStateException("无法创建原生 P2P 模块");
        }
    }

    public void close() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    public void transportUp(int attPayload) {
        if (handle != 0) nativeTransportUp(handle, attPayload);
    }

    public void transportDown(String reason) {
        if (handle != 0) nativeTransportDown(handle, reason);
    }

    public void onPacket(byte[] packet) {
        if (handle != 0) nativePacket(handle, packet);
    }

    public void setAttPayload(int payload) {
        if (handle != 0) nativeSetAttPayload(handle, payload);
    }

    public String queueFile(String name, long size, Reader reader) {
        if (handle == 0) return "原生模块已关闭";
        return nativeQueueFile(handle, name, size, reader);
    }

    public void accept(Writer writer) {
        if (handle != 0) nativeAccept(handle, writer);
    }

    public void reject(String reason) {
        if (handle != 0) nativeReject(handle, reason);
    }

    public void cancelSend() {
        if (handle != 0) nativeCancelSend(handle);
    }

    public void cancelReceive() {
        if (handle != 0) nativeCancelReceive(handle);
    }

    private static native long nativeCreate(Listener listener, String deviceId, String deviceName);
    private static native void nativeDestroy(long handle);
    private static native void nativeTransportUp(long handle, int attPayload);
    private static native void nativeTransportDown(long handle, String reason);
    private static native void nativePacket(long handle, byte[] packet);
    private static native void nativeSetAttPayload(long handle, int payload);
    private static native String nativeQueueFile(long handle, String name, long size, Reader reader);
    private static native void nativeAccept(long handle, Writer writer);
    private static native void nativeReject(long handle, String reason);
    private static native void nativeCancelSend(long handle);
    private static native void nativeCancelReceive(long handle);
}
