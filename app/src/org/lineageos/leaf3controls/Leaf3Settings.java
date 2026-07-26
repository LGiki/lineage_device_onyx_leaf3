package org.lineageos.leaf3controls;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

final class Leaf3Settings {
  static final String GLOBAL_REFRESH_MODE =
      "persist.sys.leaf3.refresh_mode";
  static final String ACTIVE_REFRESH_MODE =
      "sys.leaf3.active_refresh_mode";
  static final String ACTIVE_REFRESH_SOURCE =
      "sys.leaf3.active_refresh_source";
  static final String FULL_REFRESH = "sys.leaf3.full_refresh";
  static final String IDLE_POLICY = "persist.sys.leaf3.idle_policy";
  static final String CLEANUP_POLICY = "persist.sys.leaf3.cleanup_policy";

  static final String MODE_BALANCED = "balanced";
  static final String MODE_NORMAL = "normal";
  static final String MODE_SPEED = "speed";
  static final String MODE_A2 = "a2";
  static final String MODE_REGAL = "regal";

  private static final String PROFILE_PREFERENCES = "refresh_profiles";
  private static final Set<String> VALID_MODES =
      new HashSet<>(Arrays.asList(MODE_BALANCED, MODE_NORMAL, MODE_SPEED,
                                  MODE_A2, MODE_REGAL));

  private Leaf3Settings() {}

  static boolean isRefreshMode(String mode) {
    return VALID_MODES.contains(mode);
  }

  static String getProfile(Context context, String packageName) {
    if (packageName == null) {
      return "";
    }
    final String mode = profiles(context).getString(packageName, "");
    return isRefreshMode(mode) ? mode : "";
  }

  static void setProfile(Context context, String packageName, String mode) {
    final SharedPreferences.Editor editor = profiles(context).edit();
    if (isRefreshMode(mode)) {
      editor.putString(packageName, mode);
    } else {
      editor.remove(packageName);
    }
    editor.apply();
  }

  static SharedPreferences profiles(Context context) {
    return context.createDeviceProtectedStorageContext().getSharedPreferences(
        PROFILE_PREFERENCES, Context.MODE_PRIVATE);
  }

  static String modeLabel(Context context, String mode) {
    if (MODE_NORMAL.equals(mode)) {
      return context.getString(R.string.normal);
    }
    if (MODE_SPEED.equals(mode)) {
      return context.getString(R.string.speed);
    }
    if (MODE_A2.equals(mode)) {
      return context.getString(R.string.a2);
    }
    if (MODE_REGAL.equals(mode)) {
      return context.getString(R.string.regal);
    }
    return context.getString(R.string.balanced);
  }

  static String nextMode(String mode) {
    if (MODE_BALANCED.equals(mode)) {
      return MODE_NORMAL;
    }
    if (MODE_NORMAL.equals(mode)) {
      return MODE_SPEED;
    }
    if (MODE_SPEED.equals(mode)) {
      return MODE_A2;
    }
    if (MODE_A2.equals(mode)) {
      return MODE_REGAL;
    }
    return MODE_BALANCED;
  }
}
