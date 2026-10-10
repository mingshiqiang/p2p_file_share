package com.example.appfileshare;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.UUID;

public final class DeviceIdentity {
    private static final String PREF = "fileshare";
    private static final String KEY = "deviceId";

    private DeviceIdentity() {}

    public static String id(Context context) {
        SharedPreferences preferences = context.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        String id = preferences.getString(KEY, null);
        if (id == null || id.isEmpty()) {
            id = "AFS-" + UUID.randomUUID().toString().replace("-", "").substring(0, 16);
            preferences.edit().putString(KEY, id).apply();
        }
        return id;
    }
}
