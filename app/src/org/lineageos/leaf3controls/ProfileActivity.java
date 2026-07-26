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
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class ProfileActivity extends Activity {
  private static final String[] PROFILE_MODES = {
      "", Leaf3Settings.MODE_BALANCED, Leaf3Settings.MODE_NORMAL,
      Leaf3Settings.MODE_SPEED, Leaf3Settings.MODE_A2, Leaf3Settings.MODE_REGAL
  };

  private final List<AppEntry> apps = new ArrayList<>();
  private ProfileAdapter adapter;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_profiles);

    loadApps();
    adapter = new ProfileAdapter();
    final ListView list = findViewById(R.id.profile_list);
    list.setAdapter(adapter);
    list.setOnItemClickListener((parent, view, position, id) ->
        chooseMode(apps.get(position)));
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
    final String[] labels = {
        getString(R.string.use_global_default),
        getString(R.string.balanced),
        getString(R.string.normal),
        getString(R.string.speed),
        getString(R.string.a2),
        getString(R.string.regal)
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
          adapter.notifyDataSetChanged();
          dialog.dismiss();
        })
        .setNegativeButton(android.R.string.cancel, null)
        .show();
  }

  private final class ProfileAdapter extends BaseAdapter {
    @Override
    public int getCount() {
      return apps.size();
    }

    @Override
    public AppEntry getItem(int position) {
      return apps.get(position);
    }

    @Override
    public long getItemId(int position) {
      return position;
    }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
      final LinearLayout row;
      final ImageView icon;
      final TextView text;
      if (convertView instanceof LinearLayout) {
        row = (LinearLayout) convertView;
        icon = (ImageView) row.getChildAt(0);
        text = (TextView) row.getChildAt(1);
      } else {
        row = new LinearLayout(ProfileActivity.this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(72));
        row.setPadding(dp(16), dp(8), dp(16), dp(8));

        icon = new ImageView(ProfileActivity.this);
        row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

        text = new TextView(ProfileActivity.this);
        text.setTextColor(0xff000000);
        text.setTextSize(18);
        text.setPadding(dp(16), 0, 0, 0);
        row.addView(text, new LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
      }

      final AppEntry entry = getItem(position);
      final String profile = Leaf3Settings.getProfile(
          ProfileActivity.this, entry.packageName);
      final String modeLabel = profile.isEmpty()
          ? getString(R.string.use_global_default)
          : Leaf3Settings.modeLabel(ProfileActivity.this, profile);
      icon.setImageDrawable(entry.icon);
      text.setText(getString(R.string.profile_row, entry.label, modeLabel));
      return row;
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
}
