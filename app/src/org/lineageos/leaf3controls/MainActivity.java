package org.lineageos.leaf3controls;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.provider.Settings;
import android.view.View;
import android.widget.AdapterView;
import android.widget.CompoundButton;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class MainActivity extends Activity {
  private static final int[] PAGE_INTERVALS = {1, 3, 5, 10, 30, 50, 0};
  private static final String CLEAR_ON_SLEEP =
      "persist.sys.leaf3.clear_on_sleep";
  private static final String SLEEP_SCREEN =
      "persist.sys.leaf3.sleep_screen";
  private static final String SLEEP_SCREEN_DIRECTORY = "/data/misc/leaf3";
  private static final String SLEEP_SCREEN_FILE = "sleep-screen.argb";
  private static final int PICK_SLEEP_IMAGE = 1001;
  private static final int PANEL_WIDTH = 1264;
  private static final int PANEL_HEIGHT = 1680;
  private static final String FRONTLIGHT_ENABLED =
      "persist.sys.leaf3.frontlight_enabled";
  private static final String FRONTLIGHT_BRIGHTNESS =
      "persist.sys.leaf3.frontlight_brightness";
  private static final String FRONTLIGHT_TEMPERATURE =
      "persist.sys.leaf3.frontlight_temperature";
  private static final long BACKEND_REFRESH_MILLIS = 1000;

  private boolean loading = true;
  private final Handler backendRefreshHandler =
      new Handler(Looper.getMainLooper());
  private final Runnable backendStateRefresh = new Runnable() {
    @Override
    public void run() {
      updateBackendControls();
      backendRefreshHandler.postDelayed(this, BACKEND_REFRESH_MILLIS);
    }
  };
  private SeekBar brightness;
  private SeekBar temperature;
  private SeekBar contrast;
  private SeekBar gamma;
  private Switch followAndroidBrightness;
  private RadioGroup sleepScreenModes;
  private View chooseSleepImage;
  private Switch scrollDetection;
  private RadioGroup idlePolicies;
  private TextView brightnessLabel;
  private TextView temperatureLabel;
  private TextView contrastLabel;
  private TextView gammaLabel;
  private TextView diagnostics;
  private TextView composerLimitations;
  private TextView balancedModeChoice;
  private Boolean composerBackendState;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);

    final RadioGroup refreshModes = findViewById(R.id.refresh_modes);
    idlePolicies = findViewById(R.id.idle_policies);
    final RadioGroup cleanupPolicies = findViewById(R.id.cleanup_policies);
    final RadioGroup navigation = findViewById(R.id.navigation);
    final View refreshPage = findViewById(R.id.page_refresh);
    final View tuningPage = findViewById(R.id.page_tuning);
    final View lightPage = findViewById(R.id.page_light);
    final View statusPage = findViewById(R.id.page_status);
    final Switch frontlightEnabled = findViewById(R.id.frontlight_enabled);
    final Switch disableAnimations = findViewById(R.id.disable_animations);
    sleepScreenModes = findViewById(R.id.sleep_screen_modes);
    chooseSleepImage = findViewById(R.id.choose_sleep_image);
    final Switch grayscale = findViewById(R.id.grayscale);
    scrollDetection = findViewById(R.id.scroll_detection);
    final Switch settledQuality = findViewById(R.id.settled_quality);
    final Switch dither = findViewById(R.id.dither);
    final Spinner pageInterval = findViewById(R.id.page_interval);
    contrast = findViewById(R.id.contrast);
    gamma = findViewById(R.id.gamma);
    composerLimitations = findViewById(R.id.composer_limitations);
    balancedModeChoice = findViewById(R.id.mode_balanced);
    followAndroidBrightness = findViewById(R.id.follow_android_brightness);
    brightness = findViewById(R.id.brightness);
    temperature = findViewById(R.id.temperature);
    brightnessLabel = findViewById(R.id.brightness_label);
    temperatureLabel = findViewById(R.id.temperature_label);
    contrastLabel = findViewById(R.id.contrast_label);
    gammaLabel = findViewById(R.id.gamma_label);
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
    selectSleepScreen(sleepScreenModes, sleepScreenMode());
    grayscale.setChecked(Leaf3Settings.isGrayscaleEnabled());
    scrollDetection.setChecked(
        SystemProperties.getInt(Leaf3Settings.SCROLL_DETECT, 1) != 0);
    settledQuality.setChecked(
        SystemProperties.getInt(Leaf3Settings.SETTLED_QUALITY, 1) != 0);
    dither.setChecked(SystemProperties.getInt(Leaf3Settings.DITHER, 1) != 0);
    pageInterval.setSelection(pageIntervalIndex(
        SystemProperties.getInt(Leaf3Settings.PAGE_INTERVAL, 10)));
    contrast.setProgress(
        clamp(SystemProperties.getInt(Leaf3Settings.CONTRAST, 0) + 50));
    gamma.setProgress(
        clampGamma(SystemProperties.getInt(Leaf3Settings.GAMMA, 100)) - 50);
    followAndroidBrightness.setChecked(brightnessOverride < 0);
    brightness.setProgress(brightnessOverride < 0 ? 50 : brightnessOverride);
    brightness.setEnabled(brightnessOverride >= 0);
    temperature.setProgress(
        clamp(SystemProperties.getInt(FRONTLIGHT_TEMPERATURE, 0)));
    updateBrightnessLabel();
    updateTemperatureLabel();
    updateContrastLabel(contrast.getProgress() - 50);
    updateGammaLabel(gamma.getProgress() + 50);

    updateBackendControls();
    updateDiagnostics();

    navigation.setOnCheckedChangeListener(
        new RadioGroup.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(RadioGroup group, int checkedId) {
            refreshPage.setVisibility(
                checkedId == R.id.nav_refresh ? View.VISIBLE : View.GONE);
            tuningPage.setVisibility(
                checkedId == R.id.nav_tuning ? View.VISIBLE : View.GONE);
            lightPage.setVisibility(
                checkedId == R.id.nav_light ? View.VISIBLE : View.GONE);
            statusPage.setVisibility(
                checkedId == R.id.nav_status ? View.VISIBLE : View.GONE);
            if (checkedId == R.id.nav_status) {
              updateDiagnostics();
            }
          }
        });

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

    findViewById(R.id.mode_specifications)
        .setOnClickListener(view -> showModeSpecifications());

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

    sleepScreenModes.setOnCheckedChangeListener(
        new RadioGroup.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(RadioGroup group, int checkedId) {
            if (!loading) {
              final String mode = sleepScreenModeForId(checkedId);
              if ("image".equals(mode) && !sleepScreenFile().isFile()) {
                Toast.makeText(MainActivity.this, R.string.sleep_image_missing,
                               Toast.LENGTH_LONG).show();
                selectSleepScreen(sleepScreenModes, "clear");
                return;
              }
              setProperty(SLEEP_SCREEN, mode);
            }
          }
        });

    chooseSleepImage.setOnClickListener(view -> chooseSleepImage());

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

    scrollDetection.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.SCROLL_DETECT, checked ? "1" : "0");
            }
          }
        });

    settledQuality.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.SETTLED_QUALITY, checked ? "1" : "0");
            }
          }
        });

    dither.setOnCheckedChangeListener(
        new CompoundButton.OnCheckedChangeListener() {
          @Override
          public void onCheckedChanged(CompoundButton button, boolean checked) {
            if (!loading) {
              setProperty(Leaf3Settings.DITHER, checked ? "1" : "0");
            }
          }
        });

    pageInterval.setOnItemSelectedListener(
        new AdapterView.OnItemSelectedListener() {
          @Override
          public void onItemSelected(AdapterView<?> parent, View view,
                                     int position, long id) {
            if (!loading && position >= 0 &&
                position < PAGE_INTERVALS.length) {
              setProperty(Leaf3Settings.PAGE_INTERVAL,
                          Integer.toString(PAGE_INTERVALS[position]));
            }
          }

          @Override
          public void onNothingSelected(AdapterView<?> parent) {}
        });

    contrast.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateContrastLabel(progress - 50);
      }

      @Override
      public void onStopTrackingTouch(SeekBar seekBar) {
        setProperty(Leaf3Settings.CONTRAST,
                    Integer.toString(seekBar.getProgress() - 50));
      }
    });

    gamma.setOnSeekBarChangeListener(new SimpleSeekBarListener() {
      @Override
      public void onProgressChanged(SeekBar seekBar, int progress,
                                    boolean fromUser) {
        updateGammaLabel(progress + 50);
      }

      @Override
      public void onStopTrackingTouch(SeekBar seekBar) {
        setProperty(Leaf3Settings.GAMMA,
                    Integer.toString(seekBar.getProgress() + 50));
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
    loading = false;
  }

  @Override
  protected void onResume() {
    super.onResume();
    if (diagnostics != null) {
      backendRefreshHandler.removeCallbacks(backendStateRefresh);
      backendStateRefresh.run();
      updateDiagnostics();
    }
  }

  @Override
  protected void onPause() {
    backendRefreshHandler.removeCallbacks(backendStateRefresh);
    super.onPause();
  }

  @Override
  protected void onActivityResult(int requestCode, int resultCode,
                                  Intent data) {
    super.onActivityResult(requestCode, resultCode, data);
    if (requestCode != PICK_SLEEP_IMAGE || resultCode != RESULT_OK ||
        data == null || data.getData() == null) {
      return;
    }
    final Uri image = data.getData();
    chooseSleepImage.setEnabled(false);
    new Thread(() -> importSleepImage(image)).start();
  }

  private void updateBackendControls() {
    if (composerLimitations == null || idlePolicies == null) {
      return;
    }
    final boolean composerBackend = isComposerBackend();
    final boolean backendChanged =
        composerBackendState == null ||
        composerBackendState.booleanValue() != composerBackend;
    if (!backendChanged) {
      return;
    }
    composerBackendState = composerBackend;
    composerLimitations.setVisibility(
        composerBackend ? View.VISIBLE : View.GONE);
    idlePolicies.setEnabled(!composerBackend);
    setChildrenEnabled(idlePolicies, !composerBackend);
    scrollDetection.setEnabled(!composerBackend);
    sleepScreenModes.setEnabled(!composerBackend);
    setChildrenEnabled(sleepScreenModes, !composerBackend);
    chooseSleepImage.setEnabled(!composerBackend);
    contrast.setEnabled(!composerBackend);
    gamma.setEnabled(!composerBackend);
    contrastLabel.setEnabled(!composerBackend);
    gammaLabel.setEnabled(!composerBackend);
    balancedModeChoice.setText(
        composerBackend ? R.string.balanced_explanation_composer
                        : R.string.balanced_explanation);
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

  private void updateContrastLabel(int value) {
    contrastLabel.setText(getString(R.string.contrast_value, value));
  }

  private void updateGammaLabel(int value) {
    gammaLabel.setText(getString(R.string.gamma_value, value));
  }

  private String sleepScreenMode() {
    final String mode = SystemProperties.get(SLEEP_SCREEN, "");
    if ("clear".equals(mode) || "retain".equals(mode) ||
        "image".equals(mode)) {
      return mode;
    }
    final String migrated = SystemProperties.getInt(CLEAR_ON_SLEEP, 1) != 0
        ? "clear" : "retain";
    SystemProperties.set(SLEEP_SCREEN, migrated);
    return migrated;
  }

  private void chooseSleepImage() {
    final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType("image/*");
    startActivityForResult(intent, PICK_SLEEP_IMAGE);
  }

  private void importSleepImage(Uri image) {
    try {
      writeSleepImage(image);
      runOnUiThread(() -> {
        setProperty(SLEEP_SCREEN, "image");
        selectSleepScreen(sleepScreenModes, "image");
        chooseSleepImage.setEnabled(!isComposerBackend());
        Toast.makeText(this, R.string.sleep_image_saved, Toast.LENGTH_SHORT)
            .show();
      });
    } catch (IOException | RuntimeException exception) {
      runOnUiThread(() -> {
        chooseSleepImage.setEnabled(!isComposerBackend());
        Toast.makeText(this, R.string.sleep_image_error, Toast.LENGTH_LONG)
            .show();
      });
    }
  }

  private void writeSleepImage(Uri image) throws IOException {
    final BitmapFactory.Options bounds = new BitmapFactory.Options();
    bounds.inJustDecodeBounds = true;
    try (InputStream stream = getContentResolver().openInputStream(image)) {
      if (stream == null) {
        throw new IOException("could not open image");
      }
      BitmapFactory.decodeStream(stream, null, bounds);
    }
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
      throw new IOException("not a decodable image");
    }
    final BitmapFactory.Options options = new BitmapFactory.Options();
    options.inSampleSize = 1;
    options.inPreferredConfig = Bitmap.Config.ARGB_8888;
    while (bounds.outWidth / options.inSampleSize > PANEL_WIDTH * 2 ||
           bounds.outHeight / options.inSampleSize > PANEL_HEIGHT * 2) {
      options.inSampleSize *= 2;
    }
    final Bitmap decoded;
    try (InputStream stream = getContentResolver().openInputStream(image)) {
      decoded = stream == null ? null
          : BitmapFactory.decodeStream(stream, null, options);
      if (decoded == null) {
        throw new IOException("could not decode image");
      }
    }
    final Bitmap source = decoded;
    final Bitmap panel = Bitmap.createBitmap(PANEL_WIDTH, PANEL_HEIGHT,
                                             Bitmap.Config.ARGB_8888);
    final Canvas canvas = new Canvas(panel);
    canvas.drawColor(Color.WHITE);
    final float scale = Math.min((float) PANEL_WIDTH / source.getWidth(),
                                 (float) PANEL_HEIGHT / source.getHeight());
    final float width = source.getWidth() * scale;
    final float height = source.getHeight() * scale;
    final float left = (PANEL_WIDTH - width) / 2.0f;
    final float top = (PANEL_HEIGHT - height) / 2.0f;
    final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG |
                                  Paint.DITHER_FLAG);
    canvas.drawBitmap(source, null, new RectF(left, top, left + width,
                                               top + height), paint);
    source.recycle();

    final File directory = new File(SLEEP_SCREEN_DIRECTORY);
    if (!directory.isDirectory() && !directory.mkdirs()) {
      panel.recycle();
      throw new IOException("could not create sleep-image directory");
    }
    final File output = sleepScreenFile();
    final File temporary = new File(directory, SLEEP_SCREEN_FILE + ".new");
    final int[] pixels = new int[PANEL_WIDTH];
    final byte[] row = new byte[PANEL_WIDTH * 4];
    try (BufferedOutputStream stream = new BufferedOutputStream(
             new FileOutputStream(temporary))) {
      for (int y = 0; y < PANEL_HEIGHT; ++y) {
        panel.getPixels(pixels, 0, PANEL_WIDTH, 0, y, PANEL_WIDTH, 1);
        for (int x = 0; x < PANEL_WIDTH; ++x) {
          final int color = pixels[x];
          final int gray = (((color >> 16) & 0xff) * 77 +
                            ((color >> 8) & 0xff) * 150 +
                            (color & 0xff) * 29) >> 8;
          final int offset = x * 4;
          row[offset] = (byte) gray;
          row[offset + 1] = (byte) gray;
          row[offset + 2] = (byte) gray;
          row[offset + 3] = (byte) 0xff;
        }
        stream.write(row);
      }
    } finally {
      panel.recycle();
    }
    if (!temporary.renameTo(output)) {
      temporary.delete();
      throw new IOException("could not store sleep image");
    }
  }

  private static File sleepScreenFile() {
    return new File(SLEEP_SCREEN_DIRECTORY, SLEEP_SCREEN_FILE);
  }

  private void showModeSpecifications() {
    final View content = getLayoutInflater().inflate(
        R.layout.dialog_mode_specifications, null);
    final RadioGroup modes = content.findViewById(R.id.specification_modes);
    final TextView intro = content.findViewById(R.id.specification_intro);
    final TextView details =
        content.findViewById(R.id.specification_details);
    final TextView shared = content.findViewById(R.id.specification_shared);
    final String currentMode = SystemProperties.get(
        Leaf3Settings.GLOBAL_REFRESH_MODE, Leaf3Settings.MODE_BALANCED);
    final boolean composerBackend = isComposerBackend();

    details.setText(
        getText(specificationForMode(currentMode, composerBackend)));
    if (composerBackend) {
      intro.setText(R.string.mode_specifications_intro_composer);
      shared.setText(R.string.mode_specifications_shared_composer);
    }
    selectSpecificationMode(modes, currentMode);
    modes.setOnCheckedChangeListener((group, checkedId) ->
        details.setText(
            getText(specificationForId(checkedId, composerBackend))));

    new AlertDialog.Builder(this)
        .setTitle(R.string.mode_specifications)
        .setView(content)
        .setPositiveButton(android.R.string.ok, null)
        .show();
  }

  private static int specificationForId(int checkedId,
                                        boolean composerBackend) {
    if (checkedId == R.id.spec_normal) {
      return composerBackend ? R.string.specification_normal_composer
                             : R.string.specification_normal;
    }
    if (checkedId == R.id.spec_speed) {
      return composerBackend ? R.string.specification_speed_composer
                             : R.string.specification_speed;
    }
    if (checkedId == R.id.spec_a2) {
      return composerBackend ? R.string.specification_a2_composer
                             : R.string.specification_a2;
    }
    if (checkedId == R.id.spec_regal) {
      return composerBackend ? R.string.specification_regal_composer
                             : R.string.specification_regal;
    }
    if (checkedId == R.id.spec_reader) {
      return composerBackend ? R.string.specification_reader_composer
                             : R.string.specification_reader;
    }
    return composerBackend ? R.string.specification_balanced_composer
                           : R.string.specification_balanced;
  }

  private static int specificationForMode(String mode,
                                          boolean composerBackend) {
    if (Leaf3Settings.MODE_NORMAL.equals(mode)) {
      return composerBackend ? R.string.specification_normal_composer
                             : R.string.specification_normal;
    }
    if (Leaf3Settings.MODE_SPEED.equals(mode)) {
      return composerBackend ? R.string.specification_speed_composer
                             : R.string.specification_speed;
    }
    if (Leaf3Settings.MODE_A2.equals(mode)) {
      return composerBackend ? R.string.specification_a2_composer
                             : R.string.specification_a2;
    }
    if (Leaf3Settings.MODE_REGAL.equals(mode)) {
      return composerBackend ? R.string.specification_regal_composer
                             : R.string.specification_regal;
    }
    if (Leaf3Settings.MODE_READER.equals(mode)) {
      return composerBackend ? R.string.specification_reader_composer
                             : R.string.specification_reader;
    }
    return composerBackend ? R.string.specification_balanced_composer
                           : R.string.specification_balanced;
  }

  private static void selectSpecificationMode(RadioGroup group, String mode) {
    if (Leaf3Settings.MODE_NORMAL.equals(mode)) {
      group.check(R.id.spec_normal);
    } else if (Leaf3Settings.MODE_SPEED.equals(mode)) {
      group.check(R.id.spec_speed);
    } else if (Leaf3Settings.MODE_A2.equals(mode)) {
      group.check(R.id.spec_a2);
    } else if (Leaf3Settings.MODE_REGAL.equals(mode)) {
      group.check(R.id.spec_regal);
    } else if (Leaf3Settings.MODE_READER.equals(mode)) {
      group.check(R.id.spec_reader);
    } else {
      group.check(R.id.spec_balanced);
    }
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

  private static int clampGamma(int value) {
    return Math.max(50, Math.min(200, value));
  }

  private static int pageIntervalIndex(int interval) {
    for (int index = 0; index < PAGE_INTERVALS.length; ++index) {
      if (PAGE_INTERVALS[index] == interval) {
        return index;
      }
    }
    return 3;
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
    if (checkedId == R.id.mode_reader) {
      return "reader";
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
    } else if ("reader".equals(mode)) {
      group.check(R.id.mode_reader);
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

  private static void selectSleepScreen(RadioGroup group, String mode) {
    if ("retain".equals(mode)) {
      group.check(R.id.sleep_screen_retain);
    } else if ("image".equals(mode)) {
      group.check(R.id.sleep_screen_image);
    } else {
      group.check(R.id.sleep_screen_clear);
    }
  }

  private static String sleepScreenModeForId(int checkedId) {
    if (checkedId == R.id.sleep_screen_retain) {
      return "retain";
    }
    if (checkedId == R.id.sleep_screen_image) {
      return "image";
    }
    return "clear";
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
    final long gestureScroll = getStat("scroll_gesture");
    final long hashScroll = getStat("scroll_hash");
    final long captureTime = getStat("capture_us");
    final long compareTime = getStat("compare_us");
    final long submitTime = getStat("submit_us");
    final long ioctlTime = getStat("ioctl_us");
    final long gateWaitTime = getStat("gate_wait_us");
    final long notifyCaptureTime = getStat("notify_capture_us");
    final long notifySubmitTime = getStat("notify_submit_us");
    final long notifiedCaptures = getStat("notified_captures");
    final long notifiedSubmits = getStat("notified_submits");
    final long pageTurns = getStat("page_turns");
    final long pageCleanups = getStat("page_cleanups");
    final long settledUpdates = getStat("settled");
    final long updates = partial + full;
    final int pageInterval =
        SystemProperties.getInt(Leaf3Settings.PAGE_INTERVAL, 10);
    final int contrast =
        SystemProperties.getInt(Leaf3Settings.CONTRAST, 0);
    final int gamma = clampGamma(
        SystemProperties.getInt(Leaf3Settings.GAMMA, 100));
    final boolean dither =
        SystemProperties.getInt(Leaf3Settings.DITHER, 1) != 0;

    final String bridgeDiagnostics = getString(
        R.string.diagnostics_value,
        Leaf3Settings.modeLabel(this, effectiveMode),
        source,
        captureMode,
        SystemProperties.get(Leaf3Settings.IDLE_POLICY, "balanced"),
        SystemProperties.get(Leaf3Settings.CLEANUP_POLICY, "balanced"),
        pageInterval == 0 ? "off" : Integer.toString(pageInterval),
        contrast,
        gamma,
        dither ? "on" : "off",
        captures,
        changed,
        dropped,
        scroll,
        gestureScroll,
        hashScroll,
        partial,
        full,
        split,
        bilevel,
        pageTurns,
        pageCleanups,
        settledUpdates,
        pixels,
        averageMicros(captureTime, captures),
        averageMicros(compareTime, comparisons),
        averageMicros(submitTime, updates),
        averageMicros(ioctlTime, updates),
        averageMicros(gateWaitTime, updates),
        averageMicros(notifyCaptureTime, notifiedCaptures),
        averageMicros(notifySubmitTime, notifiedSubmits));
    final String backend = SystemProperties.get(
        Leaf3Settings.EPDC_BACKEND_ACTIVE,
        SystemProperties.get(Leaf3Settings.EPDC_BACKEND, "bridge"));
    final String nativeState =
        SystemProperties.get("sys.leaf3.stat.epdc_native_state", "inactive");
    final StringBuilder status = new StringBuilder(getString(
        R.string.diagnostics_backend, backend, nativeState));
    if ("composer".equals(backend)) {
      status.append('\n').append(getString(
          R.string.diagnostics_native,
          getStat("epdc_native_commands"),
          getStat("epdc_native_pixels"),
          getStat("epdc_native_cleanup"),
          getStat("epdc_native_errors")));
    }
    status.append("\n\n").append(bridgeDiagnostics);
    diagnostics.setText(status);
  }

  private static long getStat(String name) {
    return SystemProperties.getLong("sys.leaf3.stat." + name, 0);
  }

  private static long averageMicros(long total, long count) {
    return count == 0 ? 0 : total / count;
  }

  private static boolean isComposerBackend() {
    final String activeBackend =
        SystemProperties.get(Leaf3Settings.EPDC_BACKEND_ACTIVE, "");
    return "composer".equals(activeBackend) ||
        (activeBackend.isEmpty() && "composer".equals(SystemProperties.get(
            Leaf3Settings.EPDC_BACKEND, "bridge")));
  }

  private static void setChildrenEnabled(RadioGroup group, boolean enabled) {
    for (int index = 0; index < group.getChildCount(); ++index) {
      group.getChildAt(index).setEnabled(enabled);
    }
  }

  private abstract static class SimpleSeekBarListener
      implements SeekBar.OnSeekBarChangeListener {
    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {}

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {}
  }
}
