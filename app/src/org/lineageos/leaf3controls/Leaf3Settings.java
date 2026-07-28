package org.lineageos.leaf3controls;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.ColorDisplayManager;
import android.os.SystemProperties;
import android.util.Log;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

final class Leaf3Settings {
  private static final String TAG = "Leaf3Settings";

  static final String GLOBAL_REFRESH_MODE =
      "persist.sys.leaf3.refresh_mode";
  static final String ACTIVE_REFRESH_MODE =
      "sys.leaf3.active_refresh_mode";
  static final String ACTIVE_REFRESH_SOURCE =
      "sys.leaf3.active_refresh_source";
  static final String ACTIVE_PACKAGE = "sys.leaf3.active_package";
  static final String FULL_REFRESH = "sys.leaf3.full_refresh";
  static final String IDLE_POLICY = "persist.sys.leaf3.idle_policy";
  static final String CLEANUP_POLICY = "persist.sys.leaf3.cleanup_policy";
  static final String SCROLL_DETECT = "persist.sys.leaf3.scroll_detect";
  static final String PAGE_INTERVAL = "persist.sys.leaf3.page_interval";
  static final String SETTLED_QUALITY =
      "persist.sys.leaf3.settle_quality";
  static final String CONTRAST = "persist.sys.leaf3.contrast";
  static final String GAMMA = "persist.sys.leaf3.gamma";
  static final String DITHER = "persist.sys.leaf3.dither";
  static final String GRAYSCALE = "persist.sys.leaf3.grayscale";
  static final String CAPTURE_MODE_ACTIVE = "sys.leaf3.stat.capture_mode";
  static final String EPDC_BACKEND = "persist.sys.leaf3.epdc_backend";
  static final String EPDC_BACKEND_ACTIVE =
      "sys.leaf3.stat.epdc_backend";

  private static final int SATURATION_GRAYSCALE = 0;
  private static final int SATURATION_FULL = 100;

  static final String MODE_BALANCED = "balanced";
  static final String MODE_NORMAL = "normal";
  static final String MODE_SPEED = "speed";
  static final String MODE_A2 = "a2";
  static final String MODE_REGAL = "regal";
  static final String MODE_READER = "reader";

  private static final String PROFILE_PREFERENCES = "refresh_profiles";
  private static final Set<String> VALID_MODES =
      new HashSet<>(Arrays.asList(MODE_BALANCED, MODE_NORMAL, MODE_SPEED,
                                  MODE_A2, MODE_REGAL, MODE_READER));

  private Leaf3Settings() {}

  static boolean isRefreshMode(String mode) {
    return VALID_MODES.contains(mode);
  }

  static boolean isGrayscaleEnabled() {
    return SystemProperties.getInt(GRAYSCALE, 0) != 0;
  }

  static boolean hasGrayscalePreference() {
    return !SystemProperties.get(GRAYSCALE, "").isEmpty();
  }

  // Colour maps to a narrow band of muddy mid-greys on a 16-level panel.
  // Desaturating in SurfaceFlinger's colour matrix costs nothing per frame.
  static void applyGrayscale(Context context, boolean enabled) {
    final ColorDisplayManager manager =
        context.getSystemService(ColorDisplayManager.class);
    if (manager == null) {
      Log.w(TAG, "ColorDisplayManager is unavailable");
      return;
    }
    try {
      manager.setSaturationLevel(enabled ? SATURATION_GRAYSCALE
                                         : SATURATION_FULL);
    } catch (RuntimeException exception) {
      Log.e(TAG, "Could not set the display saturation level", exception);
    }
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
    if (MODE_READER.equals(mode)) {
      return context.getString(R.string.reader);
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
    if (MODE_REGAL.equals(mode)) {
      return MODE_READER;
    }
    return MODE_BALANCED;
  }
}
