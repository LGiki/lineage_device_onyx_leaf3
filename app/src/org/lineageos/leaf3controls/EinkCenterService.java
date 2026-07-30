package org.lineageos.leaf3controls;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

public final class EinkCenterService extends Service {
  private static final String TAG = "Leaf3EinkCenter";
  private static final String PREFERENCES = "eink_center";
  private static final String MODE_SCOPE = "mode_scope";
  private static final String SCOPE_CURRENT = "current";
  private static final String SCOPE_GLOBAL = "global";
  private static final long CLEAN_DELAY_MILLIS = 120;
  private static final IBinder WINDOW_TOKEN = new Binder();

  private final Handler mainHandler = new Handler(Looper.getMainLooper());
  private final BroadcastReceiver screenReceiver = new BroadcastReceiver() {
    @Override
    public void onReceive(Context context, Intent intent) {
      hidePanel(true);
    }
  };

  private WindowManager windowManager;
  private View panelRoot;
  private boolean loading;
  private RadioGroup scopeGroup;
  private RadioGroup modeGroup;
  private Switch followBrightness;
  private SeekBar brightness;
  private SeekBar temperature;
  private TextView brightnessLabel;
  private TextView temperatureLabel;

  @Override
  public void onCreate() {
    super.onCreate();
    windowManager = getSystemService(WindowManager.class);
    registerReceiver(screenReceiver, new IntentFilter(Intent.ACTION_SCREEN_OFF));
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    if (intent != null &&
        Leaf3Settings.TOGGLE_EINK_CENTER.equals(intent.getAction())) {
      if (panelRoot == null) {
        showPanel();
      } else {
        hidePanel(true);
      }
    }
    return START_NOT_STICKY;
  }

  @Override
  public void onConfigurationChanged(Configuration configuration) {
    super.onConfigurationChanged(configuration);
    hidePanel(true);
  }

  @Override
  public void onDestroy() {
    hidePanel(false);
    try {
      unregisterReceiver(screenReceiver);
    } catch (IllegalArgumentException exception) {
      Log.w(TAG, "Screen receiver was already unregistered", exception);
    }
    super.onDestroy();
  }

  @Override
  public IBinder onBind(Intent intent) {
    return null;
  }

  private void showPanel() {
    if (windowManager == null || panelRoot != null) {
      return;
    }
    final EinkCenterRootView root =
        (EinkCenterRootView) View.inflate(this, R.layout.eink_center, null);
    panelRoot = root;
    bindPanel(root);

    root.setFocusableInTouchMode(true);
    root.setOnClickListener(view -> hidePanel(true));
    root.setDismissAction(() -> hidePanel(true));
    root.findViewById(R.id.eink_center_panel)
        .setOnClickListener(view -> {});

    final WindowManager.LayoutParams params = new WindowManager.LayoutParams(
        WindowManager.LayoutParams.MATCH_PARENT,
        WindowManager.LayoutParams.MATCH_PARENT,
        WindowManager.LayoutParams.TYPE_STATUS_BAR_SUB_PANEL,
        WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN |
            WindowManager.LayoutParams.FLAG_FULLSCREEN |
            WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM |
            WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
        PixelFormat.TRANSLUCENT);
    params.setTitle("Leaf3EinkCenter");
    params.token = WINDOW_TOKEN;
    params.windowAnimations = 0;
    try {
      windowManager.addView(root, params);
      root.requestFocus();
    } catch (RuntimeException exception) {
      panelRoot = null;
      Log.e(TAG, "Could not show the E-Ink Center", exception);
      Toast.makeText(this, R.string.eink_center_error, Toast.LENGTH_LONG).show();
      stopSelf();
    }
  }

  private void bindPanel(View root) {
    loading = true;
    scopeGroup = root.findViewById(R.id.eink_center_scope);
    modeGroup = root.findViewById(R.id.eink_center_modes);
    followBrightness = root.findViewById(R.id.eink_center_follow_brightness);
    brightness = root.findViewById(R.id.eink_center_brightness);
    temperature = root.findViewById(R.id.eink_center_temperature);
    brightnessLabel = root.findViewById(R.id.eink_center_brightness_label);
    temperatureLabel = root.findViewById(R.id.eink_center_temperature_label);

    final boolean global = SCOPE_GLOBAL.equals(preferences().getString(
        MODE_SCOPE, SCOPE_CURRENT));
    scopeGroup.check(global ? R.id.eink_center_scope_global
                            : R.id.eink_center_scope_current);
    selectDisplayedMode();

    final int brightnessOverride = SystemProperties.getInt(
        Leaf3Settings.FRONTLIGHT_BRIGHTNESS, -1);
    followBrightness.setChecked(brightnessOverride < 0);
    brightness.setEnabled(brightnessOverride >= 0);
    brightness.setProgress(brightnessOverride >= 0
        ? clamp(brightnessOverride)
        : androidBrightnessPercent());
    temperature.setProgress(clamp(SystemProperties.getInt(
        Leaf3Settings.FRONTLIGHT_TEMPERATURE, 0)));
    updateBrightnessLabel();
    updateTemperatureLabel();

    root.findViewById(R.id.eink_center_close)
        .setOnClickListener(view -> hidePanel(true));
    root.findViewById(R.id.eink_center_clean)
        .setOnClickListener(view -> cleanScreen());
    root.findViewById(R.id.eink_center_more)
        .setOnClickListener(view -> openMoreSettings());

    scopeGroup.setOnCheckedChangeListener((group, checkedId) -> {
      if (loading) {
        return;
      }
      preferences().edit()
          .putString(MODE_SCOPE,
                     checkedId == R.id.eink_center_scope_global
                         ? SCOPE_GLOBAL : SCOPE_CURRENT)
          .apply();
      loading = true;
      selectDisplayedMode();
      loading = false;
    });
    modeGroup.setOnCheckedChangeListener((group, checkedId) -> {
      if (!loading) {
        applyMode(modeForId(checkedId));
      }
    });
    followBrightness.setOnCheckedChangeListener((button, checked) -> {
      brightness.setEnabled(!checked);
      if (!loading) {
        setProperty(Leaf3Settings.FRONTLIGHT_BRIGHTNESS,
                    checked ? "-1"
                            : Integer.toString(brightness.getProgress()));
      }
    });
    brightness.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateBrightnessLabel();
        if (fromUser && !followBrightness.isChecked()) {
          setProperty(Leaf3Settings.FRONTLIGHT_BRIGHTNESS,
                      Integer.toString(progress));
        }
      }
    });
    temperature.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateTemperatureLabel();
        if (fromUser) {
          setProperty(Leaf3Settings.FRONTLIGHT_TEMPERATURE,
                      Integer.toString(progress));
        }
      }
    });
    loading = false;
  }

  private void selectDisplayedMode() {
    final boolean global =
        scopeGroup.getCheckedRadioButtonId() == R.id.eink_center_scope_global;
    String mode = global
        ? SystemProperties.get(Leaf3Settings.GLOBAL_REFRESH_MODE,
                               Leaf3Settings.MODE_BALANCED)
        : SystemProperties.get(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
    if (!Leaf3Settings.isRefreshMode(mode)) {
      mode = SystemProperties.get(Leaf3Settings.GLOBAL_REFRESH_MODE,
                                  Leaf3Settings.MODE_BALANCED);
    }
    modeGroup.check(idForMode(mode));
  }

  private void applyMode(String mode) {
    if (!Leaf3Settings.isRefreshMode(mode)) {
      return;
    }
    final boolean global =
        scopeGroup.getCheckedRadioButtonId() == R.id.eink_center_scope_global;
    if (global) {
      setProperty(Leaf3Settings.GLOBAL_REFRESH_MODE, mode);
      startStateService(Leaf3StateService.ACTION_CLEAR_TEMPORARY_MODE, null);
    } else {
      startStateService(Leaf3StateService.ACTION_TEMPORARY_MODE, mode);
    }
  }

  private void startStateService(String action, String mode) {
    final Intent intent = new Intent(this, Leaf3StateService.class)
        .setAction(action);
    if (mode != null) {
      intent.putExtra(Leaf3StateService.EXTRA_REFRESH_MODE, mode);
    }
    try {
      startService(intent);
    } catch (RuntimeException exception) {
      Log.e(TAG, "Could not update the refresh mode", exception);
      Toast.makeText(
          this, getString(R.string.property_error, "refresh mode"),
          Toast.LENGTH_LONG).show();
    }
  }

  private void cleanScreen() {
    hidePanel(true);
    mainHandler.postDelayed(
        () -> setProperty(
            Leaf3Settings.FULL_REFRESH,
            Long.toString(SystemClock.elapsedRealtimeNanos())),
        CLEAN_DELAY_MILLIS);
  }

  private void openMoreSettings() {
    hidePanel(true);
    final Intent intent = new Intent(this, MainActivity.class)
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK |
                  Intent.FLAG_ACTIVITY_CLEAR_TOP |
                  Intent.FLAG_ACTIVITY_NO_ANIMATION);
    try {
      startActivity(intent);
    } catch (RuntimeException exception) {
      Log.e(TAG, "Could not open Leaf3 Controls", exception);
    }
  }

  private void hidePanel(boolean stopService) {
    final View root = panelRoot;
    panelRoot = null;
    if (root != null && windowManager != null) {
      try {
        windowManager.removeViewImmediate(root);
      } catch (RuntimeException exception) {
        Log.w(TAG, "Could not remove the E-Ink Center", exception);
      }
    }
    if (stopService) {
      stopSelf();
    }
  }

  private SharedPreferences preferences() {
    return createDeviceProtectedStorageContext().getSharedPreferences(
        PREFERENCES, Context.MODE_PRIVATE);
  }

  private void setProperty(String name, String value) {
    try {
      SystemProperties.set(name, value);
    } catch (RuntimeException exception) {
      Log.e(TAG, "Could not set " + name, exception);
      Toast.makeText(this, getString(R.string.property_error, name),
                     Toast.LENGTH_LONG).show();
    }
  }

  private int androidBrightnessPercent() {
    final int brightnessValue = SystemProperties.getInt(
        Leaf3Settings.ANDROID_BRIGHTNESS, 128);
    return clamp((brightnessValue * 100 + 127) / 255);
  }

  private void updateBrightnessLabel() {
    if (brightnessLabel != null && brightness != null) {
      brightnessLabel.setText(getString(
          R.string.manual_brightness, brightness.getProgress()));
    }
  }

  private void updateTemperatureLabel() {
    if (temperatureLabel != null && temperature != null) {
      temperatureLabel.setText(getString(
          R.string.color_temperature, temperature.getProgress()));
    }
  }

  private static int clamp(int value) {
    return Math.max(0, Math.min(100, value));
  }

  private static int idForMode(String mode) {
    if (Leaf3Settings.MODE_NORMAL.equals(mode)) {
      return R.id.eink_center_mode_normal;
    }
    if (Leaf3Settings.MODE_SPEED.equals(mode)) {
      return R.id.eink_center_mode_speed;
    }
    if (Leaf3Settings.MODE_A2.equals(mode)) {
      return R.id.eink_center_mode_a2;
    }
    if (Leaf3Settings.MODE_REGAL.equals(mode)) {
      return R.id.eink_center_mode_regal;
    }
    if (Leaf3Settings.MODE_READER.equals(mode)) {
      return R.id.eink_center_mode_reader;
    }
    return R.id.eink_center_mode_balanced;
  }

  private static String modeForId(int checkedId) {
    if (checkedId == R.id.eink_center_mode_normal) {
      return Leaf3Settings.MODE_NORMAL;
    }
    if (checkedId == R.id.eink_center_mode_speed) {
      return Leaf3Settings.MODE_SPEED;
    }
    if (checkedId == R.id.eink_center_mode_a2) {
      return Leaf3Settings.MODE_A2;
    }
    if (checkedId == R.id.eink_center_mode_regal) {
      return Leaf3Settings.MODE_REGAL;
    }
    if (checkedId == R.id.eink_center_mode_reader) {
      return Leaf3Settings.MODE_READER;
    }
    return Leaf3Settings.MODE_BALANCED;
  }

  private abstract static class SimpleSeekBarListener
      implements SeekBar.OnSeekBarChangeListener {
    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {}

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {}
  }
}
