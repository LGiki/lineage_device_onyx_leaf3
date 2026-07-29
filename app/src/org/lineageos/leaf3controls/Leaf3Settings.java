package org.lineageos.leaf3controls;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.ColorDisplayManager;
import android.os.SystemProperties;
import android.util.Log;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

final class Leaf3Settings {
  private static final String TAG = "Leaf3Settings";

  static final String GLOBAL_REFRESH_MODE =
      "persist.sys.leaf3.refresh_mode";
  static final String ACTIVE_REFRESH_MODE =
      "sys.leaf3.active_refresh_mode";
  static final String ACTIVE_REFRESH_SOURCE =
      "sys.leaf3.active_refresh_source";
  static final String ACTIVE_CONTRAST = "sys.leaf3.active_contrast";
  static final String ACTIVE_GAMMA = "sys.leaf3.active_gamma";
  static final String ACTIVE_DITHER = "sys.leaf3.active_dither";
  static final String ACTIVE_PAGE_INTERVAL =
      "sys.leaf3.active_page_interval";
  static final String ACTIVE_ANIMATION_FILTER =
      "sys.leaf3.active_animation_filter";
  static final String ACTIVE_PACKAGE = "sys.leaf3.active_package";
  static final String ACTIVE_UID = "sys.leaf3.active_uid";
  static final String FULL_REFRESH = "sys.leaf3.full_refresh";
  static final String NAV_REFRESH_BUTTON =
      "persist.sys.leaf3.nav_refresh_button";
  static final String NAV_REFRESH_BUTTON_CHANGED =
      "org.lineageos.leaf3controls.action.NAV_REFRESH_BUTTON_CHANGED";
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
  private static final String PROFILE_VERSION = "v2";
  static final int INHERIT = Integer.MIN_VALUE;
  static final int[] PAGE_INTERVALS = {1, 3, 5, 10, 30, 50, 0};
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

  static AppProfile getAppProfile(Context context, String packageName) {
    if (packageName == null) {
      return new AppProfile();
    }
    return AppProfile.decode(profiles(context).getString(packageName, ""));
  }

  static void setAppProfile(Context context, String packageName,
                            AppProfile profile) {
    if (packageName == null) {
      return;
    }
    final SharedPreferences.Editor editor = profiles(context).edit();
    if (profile != null && !profile.isEmpty()) {
      editor.putString(packageName, profile.encode());
    } else {
      editor.remove(packageName);
    }
    editor.apply();
  }

  static boolean isPageInterval(int interval) {
    for (int supported : PAGE_INTERVALS) {
      if (interval == supported) {
        return true;
      }
    }
    return false;
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

  static final class AppProfile {
    String mode = "";
    int contrast = INHERIT;
    int gamma = INHERIT;
    int dither = INHERIT;
    int pageInterval = INHERIT;
    boolean filterAnimations;

    boolean isEmpty() {
      return mode.isEmpty() && contrast == INHERIT && gamma == INHERIT &&
          dither == INHERIT && pageInterval == INHERIT && !filterAnimations;
    }

    boolean hasTuning() {
      return contrast != INHERIT || gamma != INHERIT || dither != INHERIT ||
          pageInterval != INHERIT || filterAnimations;
    }

    private String encode() {
      return String.format(
          Locale.ROOT, "%s|%s|%d|%d|%d|%d|%d", PROFILE_VERSION, mode,
          contrast, gamma, dither, pageInterval, filterAnimations ? 1 : 0);
    }

    private static AppProfile decode(String value) {
      final AppProfile profile = new AppProfile();
      if (value == null || value.isEmpty()) {
        return profile;
      }
      // Version-one preferences stored only the waveform name.
      if (isRefreshMode(value)) {
        profile.mode = value;
        return profile;
      }

      final String[] fields = value.split("\\|", -1);
      if (fields.length != 7 || !PROFILE_VERSION.equals(fields[0])) {
        return profile;
      }
      profile.mode = isRefreshMode(fields[1]) ? fields[1] : "";
      profile.contrast = boundedOrInherit(fields[2], -50, 50);
      profile.gamma = boundedOrInherit(fields[3], 50, 200);
      profile.dither = booleanOrInherit(fields[4]);
      profile.pageInterval = pageIntervalOrInherit(fields[5]);
      profile.filterAnimations = "1".equals(fields[6]);
      return profile;
    }

    private static int boundedOrInherit(String value, int minimum,
                                        int maximum) {
      final int parsed = parseInteger(value, INHERIT);
      if (parsed == INHERIT) {
        return INHERIT;
      }
      return Math.max(minimum, Math.min(maximum, parsed));
    }

    private static int booleanOrInherit(String value) {
      final int parsed = parseInteger(value, INHERIT);
      return parsed == 0 || parsed == 1 ? parsed : INHERIT;
    }

    private static int pageIntervalOrInherit(String value) {
      final int parsed = parseInteger(value, INHERIT);
      return isPageInterval(parsed) ? parsed : INHERIT;
    }

    private static int parseInteger(String value, int fallback) {
      try {
        return Integer.parseInt(value);
      } catch (NumberFormatException exception) {
        return fallback;
      }
    }
  }
}
