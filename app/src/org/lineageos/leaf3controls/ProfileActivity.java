package org.lineageos.leaf3controls;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.SystemProperties;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class ProfileActivity extends Activity {
  private static final int APPS_PER_PAGE = 6;
  private static final String[] PROFILE_MODES = {
      "", Leaf3Settings.MODE_BALANCED, Leaf3Settings.MODE_NORMAL,
      Leaf3Settings.MODE_SPEED, Leaf3Settings.MODE_A2, Leaf3Settings.MODE_REGAL,
      Leaf3Settings.MODE_READER
  };

  private final List<AppEntry> apps = new ArrayList<>();
  private LinearLayout appList;
  private Button previousButton;
  private Button nextButton;
  private TextView pageLabel;
  private int page;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_profiles);

    loadApps();
    appList = findViewById(R.id.profile_list);
    previousButton = findViewById(R.id.profile_previous);
    nextButton = findViewById(R.id.profile_next);
    pageLabel = findViewById(R.id.profile_page);
    previousButton.setOnClickListener(view -> showPage(page - 1));
    nextButton.setOnClickListener(view -> showPage(page + 1));
    showPage(0);
  }

  private void showPage(int requestedPage) {
    final int pageCount = Math.max(
        1, (apps.size() + APPS_PER_PAGE - 1) / APPS_PER_PAGE);
    page = Math.max(0, Math.min(requestedPage, pageCount - 1));
    previousButton.setEnabled(page > 0);
    nextButton.setEnabled(page + 1 < pageCount);
    pageLabel.setText(getString(R.string.profile_page, page + 1, pageCount));
    renderPage();
  }

  private void renderPage() {
    appList.removeAllViews();
    final int first = page * APPS_PER_PAGE;
    final int last = Math.min(first + APPS_PER_PAGE, apps.size());
    for (int index = first; index < last; ++index) {
      if (index > first) {
        final View divider = new View(this);
        divider.setBackgroundColor(0x66000000);
        appList.addView(divider, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
      }
      appList.addView(createAppRow(apps.get(index)));
    }
  }

  private View createAppRow(AppEntry entry) {
    final LinearLayout row = new LinearLayout(this);
    row.setOrientation(LinearLayout.HORIZONTAL);
    row.setGravity(Gravity.CENTER_VERTICAL);
    row.setMinimumHeight(dp(72));
    row.setPadding(dp(16), dp(8), dp(16), dp(8));
    row.setOnClickListener(view -> editProfile(entry));

    final ImageView icon = new ImageView(this);
    icon.setImageDrawable(entry.icon);
    row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

    final Leaf3Settings.AppProfile profile =
        Leaf3Settings.getAppProfile(this, entry.packageName);
    final String modeLabel = profile.mode.isEmpty()
        ? getString(R.string.use_global_default)
        : Leaf3Settings.modeLabel(this, profile.mode);
    final String tuningLabel = profile.hasTuning()
        ? getString(R.string.profile_custom_tuning)
        : getString(R.string.profile_no_custom_tuning);
    final TextView text = new TextView(this);
    text.setText(getString(R.string.profile_row_details, entry.label,
                           modeLabel, tuningLabel));
    text.setTextColor(0xff000000);
    text.setTextSize(18);
    text.setPadding(dp(16), 0, 0, 0);
    row.addView(text, new LinearLayout.LayoutParams(
        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
    return row;
  }

  private void loadApps() {
    final PackageManager packageManager = getPackageManager();
    final Intent launcherIntent = new Intent(Intent.ACTION_MAIN);
    launcherIntent.addCategory(Intent.CATEGORY_LAUNCHER);
    final List<ResolveInfo> resolved =
        packageManager.queryIntentActivities(launcherIntent, 0);
    final Set<String> seenPackages = new HashSet<>();
    final Set<String> installedPackages = new HashSet<>();

    for (ResolveInfo info : resolved) {
      final String packageName = info.activityInfo.packageName;
      installedPackages.add(packageName);
      if (getPackageName().equals(packageName) ||
          !seenPackages.add(packageName)) {
        continue;
      }
      final CharSequence label = info.loadLabel(packageManager);
      final Drawable icon = info.loadIcon(packageManager);
      apps.add(new AppEntry(packageName, label.toString(), icon));
    }

    Collections.sort(apps, Comparator.comparing(
        entry -> entry.label, String.CASE_INSENSITIVE_ORDER));

    final SharedPreferences preferences = Leaf3Settings.profiles(this);
    final SharedPreferences.Editor editor = preferences.edit();
    boolean changed = false;
    for (String packageName : preferences.getAll().keySet()) {
      if (!installedPackages.contains(packageName)) {
        editor.remove(packageName);
        changed = true;
      }
    }
    if (changed) {
      editor.apply();
    }
  }

  private void editProfile(AppEntry entry) {
    final Leaf3Settings.AppProfile current =
        Leaf3Settings.getAppProfile(this, entry.packageName);
    final View content = getLayoutInflater().inflate(
        R.layout.dialog_app_profile, null);
    final Spinner mode = content.findViewById(R.id.profile_mode);
    final Switch overrideContrast =
        content.findViewById(R.id.profile_override_contrast);
    final SeekBar contrast = content.findViewById(R.id.profile_contrast);
    final Switch overrideGamma =
        content.findViewById(R.id.profile_override_gamma);
    final SeekBar gamma = content.findViewById(R.id.profile_gamma);
    final TextView contrastLabel =
        content.findViewById(R.id.profile_contrast_label);
    final TextView gammaLabel =
        content.findViewById(R.id.profile_gamma_label);
    final Switch overrideDither =
        content.findViewById(R.id.profile_override_dither);
    final Switch dither = content.findViewById(R.id.profile_dither);
    final Switch overrideCleanup =
        content.findViewById(R.id.profile_override_cleanup);
    final Spinner pageInterval =
        content.findViewById(R.id.profile_page_interval);
    final Switch filterAnimations =
        content.findViewById(R.id.profile_filter_animations);

    mode.setSelection(modeIndex(current.mode));
    overrideContrast.setChecked(current.contrast != Leaf3Settings.INHERIT);
    contrast.setProgress((current.contrast == Leaf3Settings.INHERIT
        ? SystemProperties.getInt(Leaf3Settings.CONTRAST, 0)
        : current.contrast) + 50);
    setControlsEnabled(overrideContrast.isChecked(), contrast, contrastLabel);
    overrideContrast.setOnCheckedChangeListener((button, checked) ->
        setControlsEnabled(checked, contrast, contrastLabel));

    overrideGamma.setChecked(current.gamma != Leaf3Settings.INHERIT);
    gamma.setProgress((current.gamma == Leaf3Settings.INHERIT
        ? SystemProperties.getInt(Leaf3Settings.GAMMA, 100)
        : current.gamma) - 50);
    setControlsEnabled(overrideGamma.isChecked(), gamma, gammaLabel);
    overrideGamma.setOnCheckedChangeListener((button, checked) ->
        setControlsEnabled(checked, gamma, gammaLabel));

    overrideDither.setChecked(current.dither != Leaf3Settings.INHERIT);
    dither.setChecked(current.dither == Leaf3Settings.INHERIT
        ? SystemProperties.getInt(Leaf3Settings.DITHER, 1) != 0
        : current.dither != 0);
    dither.setEnabled(overrideDither.isChecked());
    overrideDither.setOnCheckedChangeListener(
        (button, checked) -> dither.setEnabled(checked));

    updateToneLabels(contrast, gamma, contrastLabel, gammaLabel);
    contrast.setOnSeekBarChangeListener(
        new LabelSeekBarListener(() ->
            updateToneLabels(contrast, gamma, contrastLabel, gammaLabel)));
    gamma.setOnSeekBarChangeListener(
        new LabelSeekBarListener(() ->
            updateToneLabels(contrast, gamma, contrastLabel, gammaLabel)));

    overrideCleanup.setChecked(
        current.pageInterval != Leaf3Settings.INHERIT);
    pageInterval.setSelection(pageIntervalIndex(current.pageInterval));
    pageInterval.setEnabled(overrideCleanup.isChecked());
    overrideCleanup.setOnCheckedChangeListener(
        (button, checked) -> pageInterval.setEnabled(checked));
    filterAnimations.setChecked(current.filterAnimations);

    final AlertDialog dialog = new AlertDialog.Builder(this)
        .setTitle(entry.label)
        .setView(content)
        .setPositiveButton(android.R.string.ok, (ignored, which) -> {
          final Leaf3Settings.AppProfile updated =
              new Leaf3Settings.AppProfile();
          updated.mode = PROFILE_MODES[mode.getSelectedItemPosition()];
          if (overrideContrast.isChecked()) {
            updated.contrast = contrast.getProgress() - 50;
          }
          if (overrideGamma.isChecked()) {
            updated.gamma = gamma.getProgress() + 50;
          }
          if (overrideDither.isChecked()) {
            updated.dither = dither.isChecked() ? 1 : 0;
          }
          if (overrideCleanup.isChecked()) {
            updated.pageInterval = Leaf3Settings.PAGE_INTERVALS[
                pageInterval.getSelectedItemPosition()];
          }
          updated.filterAnimations = filterAnimations.isChecked();
          saveProfile(entry.packageName, updated);
        })
        .setNeutralButton(R.string.profile_reset, (ignored, which) ->
            saveProfile(entry.packageName, new Leaf3Settings.AppProfile()))
        .setNegativeButton(android.R.string.cancel, null)
        .create();
    dialog.show();
  }

  private void saveProfile(String packageName,
                           Leaf3Settings.AppProfile profile) {
    Leaf3Settings.setAppProfile(this, packageName, profile);
    startService(new Intent(this, Leaf3StateService.class)
        .setAction(Leaf3StateService.ACTION_APPLY_SETTINGS));
    renderPage();
  }

  private static int modeIndex(String currentMode) {
    for (int index = 1; index < PROFILE_MODES.length; ++index) {
      if (PROFILE_MODES[index].equals(currentMode)) {
        return index;
      }
    }
    return 0;
  }

  private static int pageIntervalIndex(int interval) {
    for (int index = 0; index < Leaf3Settings.PAGE_INTERVALS.length; ++index) {
      if (Leaf3Settings.PAGE_INTERVALS[index] == interval) {
        return index;
      }
    }
    return 0;
  }

  private void updateToneLabels(SeekBar contrast, SeekBar gamma,
                                TextView contrastLabel,
                                TextView gammaLabel) {
    contrastLabel.setText(
        getString(R.string.contrast_value, contrast.getProgress() - 50));
    gammaLabel.setText(
        getString(R.string.gamma_value, gamma.getProgress() + 50));
  }

  private static void setControlsEnabled(boolean enabled, View... controls) {
    for (View control : controls) {
      control.setEnabled(enabled);
    }
  }

  private int dp(int value) {
    return Math.round(value * getResources().getDisplayMetrics().density);
  }

  private static final class AppEntry {
    final String packageName;
    final String label;
    final Drawable icon;

    AppEntry(String packageName, String label, Drawable icon) {
      this.packageName = packageName;
      this.label = label;
      this.icon = icon;
    }
  }

  private static final class LabelSeekBarListener
      implements SeekBar.OnSeekBarChangeListener {
    private final Runnable update;

    LabelSeekBarListener(Runnable update) {
      this.update = update;
    }

    @Override
    public void onProgressChanged(SeekBar seekBar, int progress,
                                  boolean fromUser) {
      update.run();
    }

    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {}

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {}
  }
}
