package org.lineageos.leaf3controls;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.provider.Settings;
import android.view.View;
import android.widget.CompoundButton;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity {
  private static final String CLEAR_ON_SLEEP =
      "persist.sys.leaf3.clear_on_sleep";
  private static final String FRONTLIGHT_ENABLED =
      "persist.sys.leaf3.frontlight_enabled";
  private static final String FRONTLIGHT_BRIGHTNESS =
      "persist.sys.leaf3.frontlight_brightness";
  private static final String FRONTLIGHT_TEMPERATURE =
      "persist.sys.leaf3.frontlight_temperature";

  private boolean loading = true;
  private SeekBar brightness;
  private SeekBar temperature;
  private Switch followAndroidBrightness;
  private TextView brightnessLabel;
  private TextView temperatureLabel;
  private TextView diagnostics;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);

    final RadioGroup refreshModes = findViewById(R.id.refresh_modes);
    final RadioGroup idlePolicies = findViewById(R.id.idle_policies);
    final RadioGroup cleanupPolicies = findViewById(R.id.cleanup_policies);
    final Switch frontlightEnabled = findViewById(R.id.frontlight_enabled);
    final Switch disableAnimations = findViewById(R.id.disable_animations);
    final Switch clearOnSleep = findViewById(R.id.clear_on_sleep);
    final Switch grayscale = findViewById(R.id.grayscale);
    final Switch contentAware = findViewById(R.id.content_aware);
    final Switch scrollDetection = findViewById(R.id.scroll_detection);
    followAndroidBrightness = findViewById(R.id.follow_android_brightness);
    brightness = findViewById(R.id.brightness);
    temperature = findViewById(R.id.temperature);
    brightnessLabel = findViewById(R.id.brightness_label);
    temperatureLabel = findViewById(R.id.temperature_label);
    diagnostics = findViewById(R.id.diagnostics);

    selectRefreshMode(refreshModes,
                      SystemProperties.get(
                          Leaf3Settings.GLOBAL_REFRESH_MODE,
                          Leaf3Settings.MODE_BALANCED));
    selectIdlePolicy(idlePolicies,
                     SystemProperties.get(Leaf3Settings.IDLE_POLICY,
                                          "balanced"));
    selectCleanupPolicy(
        cleanupPolicies,
        SystemProperties.get(Leaf3Settings.CLEANUP_POLICY, "balanced"));

    final int storedBrightnessOverride =
        SystemProperties.getInt(FRONTLIGHT_BRIGHTNESS, -1);
    final int brightnessOverride =
        storedBrightnessOverride < 0 ? -1 : clamp(storedBrightnessOverride);
    frontlightEnabled.setChecked(
        SystemProperties.getInt(FRONTLIGHT_ENABLED, 1) != 0);
    disableAnimations.setChecked(animationsDisabled());
    clearOnSleep.setChecked(SystemProperties.getInt(CLEAR_ON_SLEEP, 1) != 0);
    grayscale.setChecked(Leaf3Settings.isGrayscaleEnabled());
    contentAware.setChecked(
        SystemProperties.getInt(Leaf3Settings.CONTENT_AWARE, 0) != 0);
    scrollDetection.setChecked(
        SystemProperties.getInt(Leaf3Settings.SCROLL_DETECT, 0) != 0);
    followAndroidBrightness.setChecked(brightnessOverride < 0);
    brightness.setProgress(brightnessOverride < 0 ? 50 : brightnessOverride);
    brightness.setEnabled(brightnessOverride >= 0);
    temperature.setProgress(
        clamp(SystemProperties.getInt(FRONTLIGHT_TEMPERATURE, 0)));
    updateBrightnessLabel();
    updateTemperatureLabel();
    updateDiagnostics();
    loading = false;

    refreshModes.setOnCheckedChangeListener(
        new RadioGroup.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(RadioGroup group, int checkedId) {
            if (!loading) {
              setProperty(Leaf3Settings.GLOBAL_REFRESH_MODE,
                          refreshModeForId(checkedId));
            }
          }
        });

    idlePolicies.setOnCheckedChangeListener(
        new RadioGroup.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(RadioGroup group, int checkedId) {
            if (!loading) {
              setProperty(Leaf3Settings.IDLE_POLICY,
                          idlePolicyForId(checkedId));
            }
          }
        });

    cleanupPolicies.setOnCheckedChangeListener(
        new RadioGroup.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(RadioGroup group, int checkedId) {
            if (!loading) {
              setProperty(Leaf3Settings.CLEANUP_POLICY,
                          cleanupPolicyForId(checkedId));
            }
          }
        });

    findViewById(R.id.full_refresh)
        .setOnClickListener(new View.OnClickListener() {
          @Override
          public void onClick(View view) {
            setProperty(Leaf3Settings.FULL_REFRESH,
                        Long.toString(SystemClock.elapsedRealtimeNanos()));
          }
        });

    findViewById(R.id.per_app_profiles)
        .setOnClickListener(view ->
            startActivity(new Intent(this, ProfileActivity.class)));

    findViewById(R.id.refresh_diagnostics)
        .setOnClickListener(view -> updateDiagnostics());

    frontlightEnabled.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(FRONTLIGHT_ENABLED, checked ? "1" : "0");
            }
          }
        });

    disableAnimations.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setAnimationsDisabled(checked);
            }
          }
        });

    clearOnSleep.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(CLEAR_ON_SLEEP, checked ? "1" : "0");
            }
          }
        });

    grayscale.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.GRAYSCALE, checked ? "1" : "0");
              Leaf3Settings.applyGrayscale(MainActivity.this, checked);
            }
          }
        });

    contentAware.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.CONTENT_AWARE, checked ? "1" : "0");
            }
          }
        });

    scrollDetection.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.SCROLL_DETECT, checked ? "1" : "0");
            }
          }
        });

    followAndroidBrightness.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            brightness.setEnabled(!checked);
            if (!loading) {
              setProperty(FRONTLIGHT_BRIGHTNESS,
                          checked ? "-1"
                                  : Integer.toString(brightness.getProgress()));
            }
          }
        });

    brightness.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateBrightnessLabel();
        if (fromUser && !followAndroidBrightness.isChecked()) {
          setProperty(FRONTLIGHT_BRIGHTNESS, Integer.toString(progress));
        }
      }
    });

    temperature.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateTemperatureLabel();
        if (fromUser) {
          setProperty(FRONTLIGHT_TEMPERATURE, Integer.toString(progress));
        }
      }
    });
  }

  @Override
  protected void onResume() {
    super.onResume();
    if (diagnostics != null) {
      updateDiagnostics();
    }
  }

  private void updateBrightnessLabel() {
    brightnessLabel.setText(
        getString(R.string.manual_brightness, brightness.getProgress()));
  }

  private void updateTemperatureLabel() {
    temperatureLabel.setText(
        getString(R.string.color_temperature, temperature.getProgress()));
  }

  private void setProperty(String name, String value) {
    try {
      SystemProperties.set(name, value);
    } catch (RuntimeException exception) {
      Toast
          .makeText(this, getString(R.string.property_error, name),
                    Toast.LENGTH_LONG)
          .show();
    }
    updateDiagnostics();
  }

  private boolean animationsDisabled() {
    return Settings.Global.getFloat(getContentResolver(),
                                    Settings.Global.WINDOW_ANIMATION_SCALE,
                                    1.0f) == 0.0f &&
        Settings.Global.getFloat(getContentResolver(),
                                 Settings.Global.TRANSITION_ANIMATION_SCALE,
                                 1.0f) == 0.0f &&
        Settings.Global.getFloat(getContentResolver(),
                                 Settings.Global.ANIMATOR_DURATION_SCALE,
                                 1.0f) == 0.0f;
  }

  private void setAnimationsDisabled(boolean disabled) {
    final float scale = disabled ? 0.0f : 1.0f;
    try {
      Settings.Global.putFloat(getContentResolver(),
                               Settings.Global.WINDOW_ANIMATION_SCALE, scale);
      Settings.Global.putFloat(getContentResolver(),
                               Settings.Global.TRANSITION_ANIMATION_SCALE,
                               scale);
      Settings.Global.putFloat(getContentResolver(),
                               Settings.Global.ANIMATOR_DURATION_SCALE, scale);
    } catch (SecurityException exception) {
      Toast
          .makeText(this,
                    getString(R.string.property_error, "animation settings"),
                    Toast.LENGTH_LONG)
          .show();
    }
  }

  private static int clamp(int value) {
    return Math.max(0, Math.min(100, value));
  }

  private static String refreshModeForId(int checkedId) {
    if (checkedId == R.id.mode_normal) {
      return "normal";
    }
    if (checkedId == R.id.mode_speed) {
      return "speed";
    }
    if (checkedId == R.id.mode_a2) {
      return "a2";
    }
    if (checkedId == R.id.mode_regal) {
      return "regal";
    }
    return "balanced";
  }

  private static String idlePolicyForId(int checkedId) {
    if (checkedId == R.id.idle_responsive) {
      return "responsive";
    }
    if (checkedId == R.id.idle_battery) {
      return "battery";
    }
    return "balanced";
  }

  private static String cleanupPolicyForId(int checkedId) {
    if (checkedId == R.id.cleanup_quality) {
      return "quality";
    }
    if (checkedId == R.id.cleanup_manual) {
      return "manual";
    }
    return "balanced";
  }

  private static void selectRefreshMode(RadioGroup group, String mode) {
    if ("normal".equals(mode)) {
      group.check(R.id.mode_normal);
    } else if ("speed".equals(mode)) {
      group.check(R.id.mode_speed);
    } else if ("a2".equals(mode)) {
      group.check(R.id.mode_a2);
    } else if ("regal".equals(mode)) {
      group.check(R.id.mode_regal);
    } else {
      group.check(R.id.mode_balanced);
    }
  }

  private static void selectIdlePolicy(RadioGroup group, String policy) {
    if ("responsive".equals(policy)) {
      group.check(R.id.idle_responsive);
    } else if ("battery".equals(policy)) {
      group.check(R.id.idle_battery);
    } else {
      group.check(R.id.idle_balanced);
    }
  }

  private static void selectCleanupPolicy(RadioGroup group, String policy) {
    if ("quality".equals(policy)) {
      group.check(R.id.cleanup_quality);
    } else if ("manual".equals(policy)) {
      group.check(R.id.cleanup_manual);
    } else {
      group.check(R.id.cleanup_balanced);
    }
  }

  private void updateDiagnostics() {
    final String globalMode = SystemProperties.get(
        Leaf3Settings.GLOBAL_REFRESH_MODE, Leaf3Settings.MODE_BALANCED);
    final String activeMode =
        SystemProperties.get(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
    final String effectiveMode =
        Leaf3Settings.isRefreshMode(activeMode) ? activeMode : globalMode;
    final String source = SystemProperties.get(
        Leaf3Settings.ACTIVE_REFRESH_SOURCE, "default");
    final String captureMode = SystemProperties.get(
        Leaf3Settings.CAPTURE_MODE_ACTIVE, "starting");
    final long captures = getStat("captures");
    final long comparisons = getStat("comparisons");
    final long changed = getStat("changed");
    final long partial = getStat("partial");
    final long full = getStat("full");
    final long pixels = getStat("pixels");
    final long dropped = getStat("dropped");
    final long split = getStat("split");
    final long bilevel = getStat("bilevel");
    final long scroll = getStat("scroll");
    final long captureTime = getStat("capture_us");
    final long compareTime = getStat("compare_us");
    final long submitTime = getStat("submit_us");
    final long updates = partial + full;

    diagnostics.setText(getString(
        R.string.diagnostics_value,
        Leaf3Settings.modeLabel(this, effectiveMode),
        source,
        captureMode,
        SystemProperties.get(Leaf3Settings.IDLE_POLICY, "balanced"),
        SystemProperties.get(Leaf3Settings.CLEANUP_POLICY, "balanced"),
        captures,
        changed,
        dropped,
        scroll,
        partial,
        full,
        split,
        bilevel,
        pixels,
        averageMicros(captureTime, captures),
        averageMicros(compareTime, comparisons),
        averageMicros(submitTime, updates)));
  }

  private static long getStat(String name) {
    return SystemProperties.getLong("sys.leaf3.stat." + name, 0);
  }

  private static long averageMicros(long total, long count) {
    return count == 0 ? 0 : total / count;
  }

  private abstract static class SimpleSeekBarListener
      implements SeekBar.OnSeekBarChangeListener {
    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {}

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {}
  }
}
