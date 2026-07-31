package org.lineageos.leaf3controls;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.Service;
import android.app.TaskStackListener;
import android.content.ComponentName;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.SystemProperties;
import android.provider.Settings;
import android.service.quicksettings.TileService;
import android.util.Log;

import java.util.List;

public final class Leaf3StateService extends Service {
  private static final String TAG = "Leaf3StateService";
  public static final String ACTION_APPLY_SETTINGS =
      "org.lineageos.leaf3controls.action.APPLY_SETTINGS";
  public static final String ACTION_TEMPORARY_MODE =
      "org.lineageos.leaf3controls.action.TEMPORARY_MODE";
  public static final String ACTION_CLEAR_TEMPORARY_MODE =
      "org.lineageos.leaf3controls.action.CLEAR_TEMPORARY_MODE";
  public static final String EXTRA_REFRESH_MODE = "refresh_mode";

  private static final String INTERACTIVE = "sys.leaf3.interactive";
  private final Handler mainHandler = new Handler(Looper.getMainLooper());
  private final BroadcastReceiver screenReceiver = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
      publishState();
    }
  };
  private final TaskStackListener taskStackListener = new TaskStackListener() {
    @Override
    public void onTaskStackChanged() {
      mainHandler.post(Leaf3StateService.this::updateForegroundPackage);
    }
  };

  private ContentObserver brightnessObserver;
  private String foregroundPackage = "";
  private int foregroundUid = -1;
  private String temporaryPackage = "";
  private String temporaryMode = "";

  @Override
  public void onCreate() {
    super.onCreate();

    final IntentFilter screenFilter = new IntentFilter();
    screenFilter.addAction(Intent.ACTION_SCREEN_ON);
    screenFilter.addAction(Intent.ACTION_SCREEN_OFF);
    registerReceiver(screenReceiver, screenFilter);

    brightnessObserver =
        new ContentObserver(new Handler(Looper.getMainLooper())) {
          @Override
          public void onChange(boolean selfChange) {
            publishBrightness();
          }
        };
    getContentResolver().registerContentObserver(
        Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS), false,
        brightnessObserver);
    try {
      ActivityTaskManager.getService().registerTaskStackListener(
          taskStackListener);
    } catch (RemoteException | RuntimeException exception) {
      Log.e(TAG, "Could not register task-stack listener", exception);
    }
    if (Leaf3Settings.hasGrayscalePreference()) {
      Leaf3Settings.applyGrayscale(this, Leaf3Settings.isGrayscaleEnabled());
    }
    completeAnimationDefaults();
    publishState();
    updateForegroundPackage();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    if (intent != null) {
      if (ACTION_TEMPORARY_MODE.equals(intent.getAction())) {
        final String requestedMode =
            intent.getStringExtra(EXTRA_REFRESH_MODE);
        if (Leaf3Settings.isRefreshMode(requestedMode)) {
          updateForegroundPackage();
          if (!foregroundPackage.isEmpty()) {
            temporaryPackage = foregroundPackage;
            temporaryMode = requestedMode;
          }
        }
      } else if (ACTION_CLEAR_TEMPORARY_MODE.equals(intent.getAction())) {
        temporaryPackage = "";
        temporaryMode = "";
      }
    }
    publishState();
    applyRefreshMode();
    TileService.requestListeningState(
        this, new ComponentName(this, RefreshModeTileService.class));
    return START_STICKY;
  }

  @Override
  public void onDestroy() {
    unregisterReceiver(screenReceiver);
    if (brightnessObserver != null) {
      getContentResolver().unregisterContentObserver(brightnessObserver);
    }
    try {
      ActivityTaskManager.getService().unregisterTaskStackListener(
          taskStackListener);
    } catch (RemoteException | RuntimeException exception) {
      Log.w(TAG, "Could not unregister task-stack listener", exception);
    }
    clearActiveRefreshMode();
    SystemProperties.set(Leaf3Settings.ACTIVE_PACKAGE, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_UID, "");
    super.onDestroy();
  }

  @Override
  public IBinder onBind(Intent intent) {
    return null;
  }

  private void publishState() {
    final PowerManager powerManager = getSystemService(PowerManager.class);
    SystemProperties.set(
        INTERACTIVE,
        powerManager != null && powerManager.isInteractive() ? "1" : "0");
    publishBrightness();
  }

  private void completeAnimationDefaults() {
    final float window =
        Settings.Global.getFloat(getContentResolver(),
                                 Settings.Global.WINDOW_ANIMATION_SCALE, 1.0f);
    final float transition =
        Settings.Global.getFloat(
            getContentResolver(), Settings.Global.TRANSITION_ANIMATION_SCALE,
            1.0f);
    final float animator =
        Settings.Global.getFloat(getContentResolver(),
                                 Settings.Global.ANIMATOR_DURATION_SCALE, 1.0f);
    // SettingsProvider exposes overlay resources for the first two scales but
    // initializes animator_duration_scale independently. Complete the E-Ink
    // default only when the first two values still identify this device's
    // animation-off default, preserving users who enabled animations.
    if (window == 0.0f && transition == 0.0f && animator != 0.0f) {
      try {
        if (Settings.Global.putFloat(
                getContentResolver(), Settings.Global.ANIMATOR_DURATION_SCALE,
                0.0f)) {
          Log.i(TAG, "Completed the E-Ink animation-off defaults");
        } else {
          Log.e(TAG, "SettingsProvider rejected the animator duration default");
        }
      } catch (SecurityException exception) {
        Log.e(TAG, "Could not set the animator duration default", exception);
      }
    }
  }

  private void publishBrightness() {
    final int brightness =
        Settings.System.getInt(getContentResolver(),
                               Settings.System.SCREEN_BRIGHTNESS, 128);
    SystemProperties.set(
        Leaf3Settings.ANDROID_BRIGHTNESS,
        Integer.toString(Math.max(0, Math.min(255, brightness))));
  }

  private void updateForegroundPackage() {
    String packageName = "";
    int packageUid = -1;
    try {
      final List<ActivityManager.RunningTaskInfo> tasks =
          ActivityTaskManager.getService().getTasks(1);
      if (tasks != null && !tasks.isEmpty()) {
        final ActivityManager.RunningTaskInfo task = tasks.get(0);
        final ComponentName topActivity = task.topActivity;
        if (topActivity != null) {
          packageName = topActivity.getPackageName();
          packageUid = getPackageManager().getPackageUidAsUser(
              packageName, task.userId);
        }
      }
    } catch (PackageManager.NameNotFoundException | RemoteException |
             RuntimeException exception) {
      Log.e(TAG, "Could not determine foreground package", exception);
    }

    final boolean packageChanged = !packageName.equals(foregroundPackage) ||
        packageUid != foregroundUid;
    if (packageChanged) {
      foregroundPackage = packageName;
      foregroundUid = packageUid;
      temporaryPackage = "";
      temporaryMode = "";
      // Make the transition fail closed while publishing the new profile.
      // Framework hooks require both this token and their matching policy.
      SystemProperties.set(Leaf3Settings.ACTIVE_PACKAGE, "");
      SystemProperties.set(Leaf3Settings.ACTIVE_UID, "");
    }
    applyRefreshMode();
    if (foregroundPackage.isEmpty() || foregroundUid < 0) {
      SystemProperties.set(Leaf3Settings.ACTIVE_PACKAGE, "");
      SystemProperties.set(Leaf3Settings.ACTIVE_UID, "");
    } else {
      SystemProperties.set(Leaf3Settings.ACTIVE_PACKAGE,
                           Integer.toHexString(foregroundPackage.hashCode()));
      // Publish authorization last so partial transitions remain denied.
      SystemProperties.set(Leaf3Settings.ACTIVE_UID,
                           Integer.toString(foregroundUid));
    }
  }

  private void applyRefreshMode() {
    if (foregroundPackage.isEmpty()) {
      clearActiveRefreshMode();
      return;
    }
    final Leaf3Settings.AppProfile profile =
        Leaf3Settings.getAppProfile(this, foregroundPackage);
    applyProfileTuning(profile);
    if (foregroundPackage.equals(temporaryPackage) &&
        Leaf3Settings.isRefreshMode(temporaryMode)) {
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, temporaryMode);
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "temporary");
      return;
    }

    if (Leaf3Settings.isRefreshMode(profile.mode)) {
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, profile.mode);
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "profile");
    } else {
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "default");
    }
  }

  private void applyProfileTuning(Leaf3Settings.AppProfile profile) {
    setOptionalProperty(Leaf3Settings.ACTIVE_CONTRAST, profile.contrast);
    setOptionalProperty(Leaf3Settings.ACTIVE_GAMMA, profile.gamma);
    setOptionalProperty(Leaf3Settings.ACTIVE_DITHER, profile.dither);
    setOptionalProperty(Leaf3Settings.ACTIVE_PAGE_INTERVAL,
                        profile.pageInterval);
    SystemProperties.set(Leaf3Settings.ACTIVE_ANIMATION_FILTER,
                         profile.filterAnimations ? "1" : "");
  }

  private static void setOptionalProperty(String name, int value) {
    SystemProperties.set(
        name, value == Leaf3Settings.INHERIT ? "" : Integer.toString(value));
  }

  private void clearActiveRefreshMode() {
    SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "default");
    SystemProperties.set(Leaf3Settings.ACTIVE_CONTRAST, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_GAMMA, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_DITHER, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_PAGE_INTERVAL, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_ANIMATION_FILTER, "");
  }
}
