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
import android.database.ContentObserver;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.SystemProperties;
import android.provider.Settings;
import android.util.Log;

import java.util.List;

public final class Leaf3StateService extends Service {
  private static final String TAG = "Leaf3StateService";
  public static final String ACTION_APPLY_SETTINGS =
      "org.lineageos.leaf3controls.action.APPLY_SETTINGS";
  public static final String ACTION_TEMPORARY_MODE =
      "org.lineageos.leaf3controls.action.TEMPORARY_MODE";
  public static final String EXTRA_REFRESH_MODE = "refresh_mode";

  private static final String INTERACTIVE = "sys.leaf3.interactive";
  private static final String ANDROID_BRIGHTNESS =
      "sys.leaf3.android_brightness";

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
    publishState();
    updateForegroundPackage();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    if (intent != null &&
        ACTION_TEMPORARY_MODE.equals(intent.getAction())) {
      final String requestedMode =
          intent.getStringExtra(EXTRA_REFRESH_MODE);
      if (Leaf3Settings.isRefreshMode(requestedMode)) {
        updateForegroundPackage();
        if (!foregroundPackage.isEmpty()) {
          temporaryPackage = foregroundPackage;
          temporaryMode = requestedMode;
        }
      }
    }
    publishState();
    applyRefreshMode();
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

  private void publishBrightness() {
    final int brightness =
        Settings.System.getInt(getContentResolver(),
                               Settings.System.SCREEN_BRIGHTNESS, 128);
    SystemProperties.set(
        ANDROID_BRIGHTNESS,
        Integer.toString(Math.max(0, Math.min(255, brightness))));
  }

  private void updateForegroundPackage() {
    String packageName = "";
    try {
      final List<ActivityManager.RunningTaskInfo> tasks =
          ActivityTaskManager.getService().getTasks(1);
      if (tasks != null && !tasks.isEmpty()) {
        final ComponentName topActivity = tasks.get(0).topActivity;
        if (topActivity != null) {
          packageName = topActivity.getPackageName();
        }
      }
    } catch (RemoteException | RuntimeException exception) {
      Log.e(TAG, "Could not determine foreground package", exception);
    }

    if (!packageName.equals(foregroundPackage)) {
      foregroundPackage = packageName;
      temporaryPackage = "";
      temporaryMode = "";
    }
    applyRefreshMode();
  }

  private void applyRefreshMode() {
    if (foregroundPackage.isEmpty()) {
      clearActiveRefreshMode();
      return;
    }
    if (foregroundPackage.equals(temporaryPackage) &&
        Leaf3Settings.isRefreshMode(temporaryMode)) {
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, temporaryMode);
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "temporary");
      return;
    }

    final String profile =
        Leaf3Settings.getProfile(this, foregroundPackage);
    if (Leaf3Settings.isRefreshMode(profile)) {
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, profile);
      SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "profile");
    } else {
      clearActiveRefreshMode();
    }
  }

  private void clearActiveRefreshMode() {
    SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
    SystemProperties.set(Leaf3Settings.ACTIVE_REFRESH_SOURCE, "default");
  }
}
