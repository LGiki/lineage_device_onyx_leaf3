package org.lineageos.leaf3controls;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemProperties;
import android.provider.Settings;

public final class Leaf3StateService extends Service {
  private static final String INTERACTIVE = "sys.leaf3.interactive";
  private static final String ANDROID_BRIGHTNESS =
      "sys.leaf3.android_brightness";

  private final BroadcastReceiver screenReceiver = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
      publishState();
    }
  };

  private ContentObserver brightnessObserver;

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
    publishState();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    publishState();
    return START_STICKY;
  }

  @Override
  public void onDestroy() {
    unregisterReceiver(screenReceiver);
    if (brightnessObserver != null) {
      getContentResolver().unregisterContentObserver(brightnessObserver);
    }
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
}
