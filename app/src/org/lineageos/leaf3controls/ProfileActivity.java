package org.lineageos.leaf3controls;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
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
    row.setOnClickListener(view -> chooseMode(entry));

    final ImageView icon = new ImageView(this);
    icon.setImageDrawable(entry.icon);
    row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

    final String profile = Leaf3Settings.getProfile(this, entry.packageName);
    final String modeLabel = profile.isEmpty()
        ? getString(R.string.use_global_default)
        : Leaf3Settings.modeLabel(this, profile);
    final TextView text = new TextView(this);
    text.setText(getString(R.string.profile_row, entry.label, modeLabel));
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

  private void chooseMode(AppEntry entry) {
    final CharSequence[] labels = {
        getText(R.string.use_global_default_explanation),
        getText(R.string.balanced_explanation),
        getText(R.string.normal_explanation),
        getText(R.string.speed_explanation),
        getText(R.string.a2_explanation),
        getText(R.string.regal_explanation),
        getText(R.string.reader_explanation)
    };
    final String currentMode = Leaf3Settings.getProfile(this, entry.packageName);
    int checkedItem = 0;
    for (int index = 1; index < PROFILE_MODES.length; ++index) {
      if (PROFILE_MODES[index].equals(currentMode)) {
        checkedItem = index;
        break;
      }
    }

    new AlertDialog.Builder(this)
        .setTitle(entry.label)
        .setSingleChoiceItems(labels, checkedItem, (dialog, which) -> {
          Leaf3Settings.setProfile(this, entry.packageName,
                                   PROFILE_MODES[which]);
          renderPage();
          dialog.dismiss();
        })
        .setNegativeButton(android.R.string.cancel, null)
        .show();
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
}
