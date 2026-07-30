#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Add Leaf3's optional E-Ink Center and refresh navigation keys."""

import argparse
from pathlib import Path


SYSTEM_CLOCK_IMPORT = "import android.os.SystemClock;\n"
SYSTEM_PROPERTIES_IMPORT = "import android.os.SystemProperties;\n"
CONTENT_IMPORT_MARKER = "import android.content.Context;\n"
BROADCAST_IMPORTS = (
    "import android.content.BroadcastReceiver;\n"
    "import android.content.Intent;\n"
    "import android.content.IntentFilter;\n"
)
COLOR_IMPORT = "import android.graphics.Color;\n"
KEY_BUTTON_DRAWABLE_IMPORT = (
    "import com.android.systemui.statusbar.policy.KeyButtonDrawable;\n"
)
IMPORT_MARKER = "import android.graphics.drawable.Icon;\n"
KEY_BUTTON_VIEW_IMPORT = (
    "import com.android.systemui.statusbar.policy.KeyButtonView;\n"
)
CONSTANT_MARKER = '    public static final String IME_SWITCHER = "ime_switcher";\n'
CONSTRUCTOR_MARKER = (
    "        mNavBarMode = Dependency.get(NavigationModeController.class).addListener(this);\n"
)
DEFAULT_LAYOUT_MARKER = (
    "    protected String getDefaultLayout() {\n"
    "        final int defaultResource = QuickStepContract.isGesturalMode(mNavBarMode)\n"
)
CREATE_VIEW_MARKER = (
    "        } else if (IME_SWITCHER.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.ime_switcher, parent, false);\n"
)

CONSTANTS = (
    "\n"
    "    public static final String LEAF3_REFRESH = \"leaf3_refresh\";\n"
    "    public static final String LEAF3_EINK_CENTER = \"leaf3_eink_center\";\n"
    "    private static final String LEAF3_NAV_REFRESH_BUTTON =\n"
    "            \"persist.sys.leaf3.nav_refresh_button\";\n"
    "    private static final String LEAF3_NAV_EINK_CENTER_BUTTON =\n"
    "            \"persist.sys.leaf3.nav_eink_center_button\";\n"
    "    private static final String LEAF3_FULL_REFRESH = \"sys.leaf3.full_refresh\";\n"
    "    private static final String LEAF3_NAV_REFRESH_CHANGED =\n"
    "            \"org.lineageos.leaf3controls.action.NAV_REFRESH_BUTTON_CHANGED\";\n"
    "    private static final String LEAF3_NAV_EINK_CENTER_CHANGED =\n"
    "            \"org.lineageos.leaf3controls.action.NAV_EINK_CENTER_BUTTON_CHANGED\";\n"
    "    private static final String LEAF3_TOGGLE_EINK_CENTER =\n"
    "            \"org.lineageos.leaf3controls.action.TOGGLE_EINK_CENTER\";\n"
    "    private static final String LEAF3_CONTROLS_PACKAGE =\n"
    "            \"org.lineageos.leaf3controls\";\n"
    "    private static final String LEAF3_EINK_CENTER_SERVICE =\n"
    "            \"org.lineageos.leaf3controls.EinkCenterService\";\n"
)
OLD_CONSTANT_END = (
    "    private static final String LEAF3_FULL_REFRESH = \"sys.leaf3.full_refresh\";\n"
)
ACTION_CONSTANT = (
    "    private static final String LEAF3_NAV_REFRESH_CHANGED =\n"
    "            \"org.lineageos.leaf3controls.action.NAV_REFRESH_BUTTON_CHANGED\";\n"
)
PRE_CENTER_CONSTANTS = (
    "\n"
    "    public static final String LEAF3_REFRESH = \"leaf3_refresh\";\n"
    "    private static final String LEAF3_NAV_REFRESH_BUTTON =\n"
    "            \"persist.sys.leaf3.nav_refresh_button\";\n"
    "    private static final String LEAF3_FULL_REFRESH = \"sys.leaf3.full_refresh\";\n"
    "    private static final String LEAF3_NAV_REFRESH_CHANGED =\n"
    "            \"org.lineageos.leaf3controls.action.NAV_REFRESH_BUTTON_CHANGED\";\n"
)
FIELD = (
    "    private boolean mLeaf3RefreshButtonEnabled;\n"
    "    private boolean mLeaf3EinkCenterButtonEnabled;\n"
    "    private boolean mLeaf3RefreshReceiverRegistered;\n"
    "    private final BroadcastReceiver mLeaf3RefreshReceiver = new BroadcastReceiver() {\n"
    "        @Override\n"
    "        public void onReceive(Context context, Intent intent) {\n"
    "            updateLeaf3NavigationButtons();\n"
    "        }\n"
    "    };\n"
)
PRE_CENTER_FIELD = (
    "    private boolean mLeaf3RefreshButtonEnabled;\n"
    "    private boolean mLeaf3RefreshReceiverRegistered;\n"
    "    private final BroadcastReceiver mLeaf3RefreshReceiver = new BroadcastReceiver() {\n"
    "        @Override\n"
    "        public void onReceive(Context context, Intent intent) {\n"
    "            updateLeaf3RefreshButton();\n"
    "        }\n"
    "    };\n"
)
OLD_FIELD = "    private boolean mLeaf3RefreshButtonEnabled;\n"
CONSTRUCTOR = (
    "        mLeaf3RefreshButtonEnabled = leaf3RefreshButtonEnabled();\n"
    "        mLeaf3EinkCenterButtonEnabled = leaf3EinkCenterButtonEnabled();\n"
)
PRE_CENTER_CONSTRUCTOR = (
    "        mLeaf3RefreshButtonEnabled = leaf3RefreshButtonEnabled();\n"
)
OLD_CONSTRUCTOR = (
    "        mLeaf3RefreshButtonEnabled = leaf3RefreshButtonEnabled();\n"
    "        SystemProperties.addChangeCallback(() -> post(() -> {\n"
    "            final boolean enabled = leaf3RefreshButtonEnabled();\n"
    "            if (enabled != mLeaf3RefreshButtonEnabled) {\n"
    "                mLeaf3RefreshButtonEnabled = enabled;\n"
    "                onLikelyDefaultLayoutChange();\n"
    "            }\n"
    "        }));\n"
)
DEFAULT_LAYOUT = (
    "    protected String getDefaultLayout() {\n"
    "        if (!QuickStepContract.isGesturalMode(mNavBarMode)) {\n"
    "            if (mLeaf3EinkCenterButtonEnabled && mLeaf3RefreshButtonEnabled) {\n"
    "                return getContext().getString(R.string.config_navBarLayoutLeaf3CenterRefresh);\n"
    "            }\n"
    "            if (mLeaf3EinkCenterButtonEnabled) {\n"
    "                return getContext().getString(R.string.config_navBarLayoutLeaf3Center);\n"
    "            }\n"
    "            if (mLeaf3RefreshButtonEnabled) {\n"
    "                return getContext().getString(R.string.config_navBarLayoutLeaf3Refresh);\n"
    "            }\n"
    "        }\n"
    "        final int defaultResource = QuickStepContract.isGesturalMode(mNavBarMode)\n"
)
PRE_CENTER_DEFAULT_LAYOUT = (
    "    protected String getDefaultLayout() {\n"
    "        if (!QuickStepContract.isGesturalMode(mNavBarMode)\n"
    "                && mLeaf3RefreshButtonEnabled) {\n"
    "            return getContext().getString(R.string.config_navBarLayoutLeaf3Refresh);\n"
    "        }\n"
    "        final int defaultResource = QuickStepContract.isGesturalMode(mNavBarMode)\n"
)
HELPER = (
    "\n"
    "    private boolean leaf3RefreshButtonEnabled() {\n"
    "        return SystemProperties.getInt(LEAF3_NAV_REFRESH_BUTTON, 0) != 0;\n"
    "    }\n"
    "\n"
    "    private boolean leaf3EinkCenterButtonEnabled() {\n"
    "        return SystemProperties.getInt(LEAF3_NAV_EINK_CENTER_BUTTON, 0) != 0;\n"
    "    }\n"
    "\n"
    "    private void updateLeaf3NavigationButtons() {\n"
    "        final boolean refreshEnabled = leaf3RefreshButtonEnabled();\n"
    "        final boolean centerEnabled = leaf3EinkCenterButtonEnabled();\n"
    "        if (refreshEnabled != mLeaf3RefreshButtonEnabled\n"
    "                || centerEnabled != mLeaf3EinkCenterButtonEnabled) {\n"
    "            mLeaf3RefreshButtonEnabled = refreshEnabled;\n"
    "            mLeaf3EinkCenterButtonEnabled = centerEnabled;\n"
    "            onLikelyDefaultLayoutChange();\n"
    "        }\n"
    "    }\n"
)
PRE_CENTER_HELPER = (
    "\n"
    "    private boolean leaf3RefreshButtonEnabled() {\n"
    "        return SystemProperties.getInt(LEAF3_NAV_REFRESH_BUTTON, 0) != 0;\n"
    "    }\n"
    "\n"
    "    private void updateLeaf3RefreshButton() {\n"
    "        final boolean enabled = leaf3RefreshButtonEnabled();\n"
    "        if (enabled != mLeaf3RefreshButtonEnabled) {\n"
    "            mLeaf3RefreshButtonEnabled = enabled;\n"
    "            onLikelyDefaultLayoutChange();\n"
    "        }\n"
    "    }\n"
)
OLD_HELPER = (
    "\n"
    "    private boolean leaf3RefreshButtonEnabled() {\n"
    "        return SystemProperties.getInt(LEAF3_NAV_REFRESH_BUTTON, 0) != 0;\n"
    "    }\n"
)
DETACH_MARKER = (
    "    @Override\n"
    "    protected void onDetachedFromWindow() {\n"
)
ATTACH_MARKER = (
    "    @Override\n"
    "    protected void onAttachedToWindow() {\n"
)
ATTACH_SUPER_MARKER = "        super.onAttachedToWindow();\n"
REGISTER_RECEIVER = (
    "        if (!mLeaf3RefreshReceiverRegistered) {\n"
    "            final IntentFilter leaf3NavigationFilter =\n"
    "                    new IntentFilter(LEAF3_NAV_REFRESH_CHANGED);\n"
    "            leaf3NavigationFilter.addAction(LEAF3_NAV_EINK_CENTER_CHANGED);\n"
    "            getContext().registerReceiver(mLeaf3RefreshReceiver,\n"
    "                    leaf3NavigationFilter);\n"
    "            mLeaf3RefreshReceiverRegistered = true;\n"
    "        }\n"
    "        updateLeaf3NavigationButtons();\n"
)
PRE_CENTER_REGISTER_RECEIVER = (
    "        if (!mLeaf3RefreshReceiverRegistered) {\n"
    "            getContext().registerReceiver(mLeaf3RefreshReceiver,\n"
    "                    new IntentFilter(LEAF3_NAV_REFRESH_CHANGED));\n"
    "            mLeaf3RefreshReceiverRegistered = true;\n"
    "        }\n"
    "        updateLeaf3RefreshButton();\n"
)
ATTACH_METHOD = (
    ATTACH_MARKER
    + ATTACH_SUPER_MARKER
    + REGISTER_RECEIVER
    + "    }\n\n"
)
UNREGISTER_RECEIVER = (
    "        if (mLeaf3RefreshReceiverRegistered) {\n"
    "            getContext().unregisterReceiver(mLeaf3RefreshReceiver);\n"
    "            mLeaf3RefreshReceiverRegistered = false;\n"
    "        }\n"
)
LIFECYCLE = ATTACH_METHOD + DETACH_MARKER + UNREGISTER_RECEIVER
OLD_CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            v.setOnClickListener(view -> SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                    Long.toString(SystemClock.elapsedRealtimeNanos())));\n"
)
PREVIOUS_CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            final KeyButtonDrawable refreshDrawable = KeyButtonDrawable.create(\n"
    "                    getContext(), R.drawable.ic_sysbar_leaf3_refresh,\n"
    "                    false /* hasShadow */);\n"
    "            refreshDrawable.setDarkIntensity(1f);\n"
    "            ((KeyButtonView) v).setImageDrawable(refreshDrawable);\n"
    "            v.setOnClickListener(view -> SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                    Long.toString(SystemClock.elapsedRealtimeNanos())));\n"
)
THEMED_CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            final KeyButtonDrawable refreshDrawable = KeyButtonDrawable.create(\n"
    "                    getContext(), R.drawable.ic_sysbar_leaf3_refresh,\n"
    "                    false /* hasShadow */);\n"
    "            refreshDrawable.setDarkIntensity(1f);\n"
    "            ((KeyButtonView) v).setImageDrawable(refreshDrawable);\n"
    "            v.setOnClickListener(view -> {\n"
    "                try {\n"
    "                    SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                            Long.toString(SystemClock.elapsedRealtimeNanos()));\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not request an E-Ink refresh\", exception);\n"
    "                }\n"
    "            });\n"
)
DIRECT_CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            v.setOnClickListener(view -> {\n"
    "                try {\n"
    "                    SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                            Long.toString(SystemClock.elapsedRealtimeNanos()));\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not request an E-Ink refresh\", exception);\n"
    "                }\n"
    "            });\n"
)
CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            final KeyButtonDrawable refreshDrawable = KeyButtonDrawable.create(\n"
    "                    getContext(), Color.BLACK, Color.BLACK,\n"
    "                    R.drawable.ic_sysbar_leaf3_refresh,\n"
    "                    false /* hasShadow */, null /* ovalBackgroundColor */);\n"
    "            ((KeyButtonView) v).setImageDrawable(refreshDrawable);\n"
    "            v.setOnClickListener(view -> {\n"
    "                try {\n"
    "                    SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                            Long.toString(SystemClock.elapsedRealtimeNanos()));\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not request an E-Ink refresh\", exception);\n"
    "                }\n"
    "            });\n"
)
THEMED_CENTER_CREATE_VIEW = (
    "        } else if (LEAF3_EINK_CENTER.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_eink_center, parent, false);\n"
    "            final KeyButtonDrawable centerDrawable = KeyButtonDrawable.create(\n"
    "                    getContext(), R.drawable.ic_sysbar_leaf3_eink_center,\n"
    "                    false /* hasShadow */);\n"
    "            centerDrawable.setDarkIntensity(1f);\n"
    "            ((KeyButtonView) v).setImageDrawable(centerDrawable);\n"
    "            v.setOnClickListener(view -> {\n"
    "                final Intent intent = new Intent(LEAF3_TOGGLE_EINK_CENTER);\n"
    "                intent.setClassName(LEAF3_CONTROLS_PACKAGE,\n"
    "                        LEAF3_EINK_CENTER_SERVICE);\n"
    "                try {\n"
    "                    getContext().startService(intent);\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not toggle the E-Ink Center\", exception);\n"
    "                }\n"
    "            });\n"
)
DIRECT_CENTER_CREATE_VIEW = (
    "        } else if (LEAF3_EINK_CENTER.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_eink_center, parent, false);\n"
    "            v.setOnClickListener(view -> {\n"
    "                final Intent intent = new Intent(LEAF3_TOGGLE_EINK_CENTER);\n"
    "                intent.setClassName(LEAF3_CONTROLS_PACKAGE,\n"
    "                        LEAF3_EINK_CENTER_SERVICE);\n"
    "                try {\n"
    "                    getContext().startService(intent);\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not toggle the E-Ink Center\", exception);\n"
    "                }\n"
    "            });\n"
)
CENTER_CREATE_VIEW = (
    "        } else if (LEAF3_EINK_CENTER.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_eink_center, parent, false);\n"
    "            final KeyButtonDrawable centerDrawable = KeyButtonDrawable.create(\n"
    "                    getContext(), Color.BLACK, Color.BLACK,\n"
    "                    R.drawable.ic_sysbar_leaf3_eink_center,\n"
    "                    false /* hasShadow */, null /* ovalBackgroundColor */);\n"
    "            ((KeyButtonView) v).setImageDrawable(centerDrawable);\n"
    "            v.setOnClickListener(view -> {\n"
    "                final Intent intent = new Intent(LEAF3_TOGGLE_EINK_CENTER);\n"
    "                intent.setClassName(LEAF3_CONTROLS_PACKAGE,\n"
    "                        LEAF3_EINK_CENTER_SERVICE);\n"
    "                try {\n"
    "                    getContext().startService(intent);\n"
    "                } catch (RuntimeException exception) {\n"
    "                    Log.e(TAG, \"Could not toggle the E-Ink Center\", exception);\n"
    "                }\n"
    "            });\n"
)


def replace_once(text: str, old: str, new: str, parser: argparse.ArgumentParser) -> str:
    if text.count(old) != 1:
        parser.error(f"expected exactly one occurrence of: {old.strip()}")
    return text.replace(old, new, 1)


def install_lifecycle(
    text: str, parser: argparse.ArgumentParser
) -> str:
    if "getContext().registerReceiver(mLeaf3RefreshReceiver," not in text:
        if ATTACH_MARKER in text:
            text = replace_once(
                text,
                ATTACH_SUPER_MARKER,
                ATTACH_SUPER_MARKER + REGISTER_RECEIVER,
                parser,
            )
        else:
            text = replace_once(
                text,
                DETACH_MARKER,
                ATTACH_METHOD + DETACH_MARKER,
                parser,
            )
    if "getContext().unregisterReceiver(mLeaf3RefreshReceiver);" not in text:
        text = replace_once(
            text,
            DETACH_MARKER,
            DETACH_MARKER + UNREGISTER_RECEIVER,
            parser,
        )
    return text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("navigation_bar_inflater", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    path = args.navigation_bar_inflater
    text = path.read_text()
    if "LEAF3_NAV_REFRESH_BUTTON" in text:
        notification_markers = (
            "import android.content.BroadcastReceiver;",
            "import android.content.Intent;",
            "import android.content.IntentFilter;",
            ACTION_CONSTANT.strip(),
            "private boolean mLeaf3RefreshReceiverRegistered;",
            "private final BroadcastReceiver mLeaf3RefreshReceiver",
            "protected void onAttachedToWindow()",
            "getContext().registerReceiver(mLeaf3RefreshReceiver,",
            "getContext().unregisterReceiver(mLeaf3RefreshReceiver);",
        )
        create_view_markers = (
            CREATE_VIEW.strip(),
            "catch (RuntimeException exception)",
            "Could not request an E-Ink refresh",
        )
        create_view_outdated = any(
            marker not in text for marker in create_view_markers
        )
        outdated = (
            any(marker not in text for marker in notification_markers)
            or (
                "private void updateLeaf3RefreshButton()" not in text
                and "private void updateLeaf3NavigationButtons()" not in text
            )
            or OLD_CONSTRUCTOR in text
        )
        changed = False
        if outdated:
            if args.check:
                parser.error(
                    f"{path}: outdated Leaf3 refresh-button notification patch"
                )
            for broadcast_import in (
                "import android.content.BroadcastReceiver;\n",
                "import android.content.Intent;\n",
                "import android.content.IntentFilter;\n",
            ):
                if broadcast_import not in text:
                    text = replace_once(
                        text,
                        CONTENT_IMPORT_MARKER,
                        CONTENT_IMPORT_MARKER + broadcast_import,
                        parser,
                    )
            if ACTION_CONSTANT not in text:
                text = replace_once(
                    text,
                    OLD_CONSTANT_END,
                    OLD_CONSTANT_END + ACTION_CONSTANT,
                    parser,
                )
            if "private boolean mLeaf3RefreshReceiverRegistered;" not in text:
                text = replace_once(text, OLD_FIELD, FIELD, parser)
            if OLD_CONSTRUCTOR in text:
                text = replace_once(
                    text, OLD_CONSTRUCTOR, CONSTRUCTOR, parser
                )
            if (
                "private void updateLeaf3RefreshButton()" not in text
                and "private void updateLeaf3NavigationButtons()" not in text
            ):
                text = replace_once(
                    text, OLD_HELPER, HELPER, parser
                )
            text = install_lifecycle(text, parser)
            changed = True
        if create_view_outdated:
            if args.check:
                parser.error(
                    f"{path}: outdated Leaf3 refresh-button click patch"
                )
            previous = None
            if OLD_CREATE_VIEW in text:
                previous = OLD_CREATE_VIEW
            elif PREVIOUS_CREATE_VIEW in text:
                previous = PREVIOUS_CREATE_VIEW
            elif THEMED_CREATE_VIEW in text:
                previous = THEMED_CREATE_VIEW
            elif DIRECT_CREATE_VIEW in text:
                previous = DIRECT_CREATE_VIEW
            if previous is not None:
                text = replace_once(text, previous, CREATE_VIEW, parser)
            elif CREATE_VIEW not in text:
                parser.error(
                    f"{path}: unrecognized Leaf3 refresh-button click patch"
                )
            changed = True

        if "LEAF3_NAV_EINK_CENTER_BUTTON" not in text:
            if args.check:
                parser.error(
                    f"{path}: E-Ink Center navigation patch is not installed"
                )
            text = replace_once(
                text, PRE_CENTER_CONSTANTS, CONSTANTS, parser
            )
            if PRE_CENTER_FIELD in text:
                text = replace_once(
                    text, PRE_CENTER_FIELD, FIELD, parser
                )
            if PRE_CENTER_CONSTRUCTOR in text:
                text = replace_once(
                    text, PRE_CENTER_CONSTRUCTOR, CONSTRUCTOR, parser
                )
            text = replace_once(
                text, PRE_CENTER_DEFAULT_LAYOUT, DEFAULT_LAYOUT, parser
            )
            if PRE_CENTER_HELPER in text:
                text = replace_once(
                    text, PRE_CENTER_HELPER, HELPER, parser
                )
            if PRE_CENTER_REGISTER_RECEIVER in text:
                text = replace_once(
                    text,
                    PRE_CENTER_REGISTER_RECEIVER,
                    REGISTER_RECEIVER,
                    parser,
                )
            if CENTER_CREATE_VIEW not in text:
                text = replace_once(
                    text, CREATE_VIEW, CREATE_VIEW + CENTER_CREATE_VIEW, parser
                )
            changed = True

        previous_center = None
        if THEMED_CENTER_CREATE_VIEW in text:
            previous_center = THEMED_CENTER_CREATE_VIEW
        elif DIRECT_CENTER_CREATE_VIEW in text:
            previous_center = DIRECT_CENTER_CREATE_VIEW
        if previous_center is not None:
            if args.check:
                parser.error(
                    f"{path}: E-Ink Center drawable assignment is outdated"
                )
            text = replace_once(
                text, previous_center, CENTER_CREATE_VIEW, parser
            )
            changed = True

        if COLOR_IMPORT not in text:
            text = replace_once(
                text, IMPORT_MARKER, COLOR_IMPORT + IMPORT_MARKER, parser
            )
            changed = True
        if KEY_BUTTON_DRAWABLE_IMPORT not in text:
            text = replace_once(
                text,
                KEY_BUTTON_VIEW_IMPORT,
                KEY_BUTTON_DRAWABLE_IMPORT + KEY_BUTTON_VIEW_IMPORT,
                parser,
            )
            changed = True

        if changed:
            path.write_text(text)
        required = (
            ("BroadcastReceiver import",
             "import android.content.BroadcastReceiver;"),
            ("Intent import", "import android.content.Intent;"),
            ("IntentFilter import", "import android.content.IntentFilter;"),
            ("SystemClock import", "import android.os.SystemClock;"),
            ("SystemProperties import", "import android.os.SystemProperties;"),
            ("Color import", "import android.graphics.Color;"),
            ("KeyButtonDrawable import",
             "import com.android.systemui.statusbar.policy.KeyButtonDrawable;"),
            ("button token",
             "public static final String LEAF3_REFRESH = \"leaf3_refresh\";"),
            ("center token",
             "public static final String LEAF3_EINK_CENTER = "
             "\"leaf3_eink_center\";"),
            ("enabled field", "private boolean mLeaf3RefreshButtonEnabled;"),
            ("center enabled field",
             "private boolean mLeaf3EinkCenterButtonEnabled;"),
            ("receiver field",
             "private boolean mLeaf3RefreshReceiverRegistered;"),
            ("change action", "LEAF3_NAV_REFRESH_CHANGED"),
            ("receiver registration",
             "getContext().registerReceiver(mLeaf3RefreshReceiver,"),
            ("receiver cleanup",
             "getContext().unregisterReceiver(mLeaf3RefreshReceiver);"),
            ("update helper",
             "private void updateLeaf3NavigationButtons()"),
            ("custom layout", "R.string.config_navBarLayoutLeaf3Refresh"),
            ("center layout", "R.string.config_navBarLayoutLeaf3Center"),
            ("combined layout",
             "R.string.config_navBarLayoutLeaf3CenterRefresh"),
            ("button layout", "R.layout.leaf3_refresh"),
            ("center button layout", "R.layout.leaf3_eink_center"),
            ("explicit black icon colors", "Color.BLACK, Color.BLACK"),
            ("drawable assignment",
             "((KeyButtonView) v).setImageDrawable(refreshDrawable);"),
            ("refresh property", "SystemProperties.set(LEAF3_FULL_REFRESH,"),
            ("click failure guard", "catch (RuntimeException exception)"),
            ("click error log", "Could not request an E-Ink refresh"),
            ("center service action", "LEAF3_TOGGLE_EINK_CENTER"),
            ("center explicit service", "intent.setClassName("),
            ("center service start", "getContext().startService(intent);"),
            ("center error log", "Could not toggle the E-Ink Center"),
        )
        missing = [name for name, marker in required if marker not in text]
        if missing:
            parser.error(
                f"{path}: incomplete Leaf3 navigation-button patch; missing: "
                + ", ".join(missing)
            )
        print(f"{path}: Leaf3 navigation buttons are installed")
        return 0

    if args.check:
        parser.error(f"{path}: Leaf3 refresh button is not installed")

    text = replace_once(text, IMPORT_MARKER,
                        COLOR_IMPORT + IMPORT_MARKER + SYSTEM_CLOCK_IMPORT
                        + SYSTEM_PROPERTIES_IMPORT,
                        parser)
    text = replace_once(
        text,
        CONTENT_IMPORT_MARKER,
        CONTENT_IMPORT_MARKER + BROADCAST_IMPORTS,
        parser,
    )
    text = replace_once(
        text,
        KEY_BUTTON_VIEW_IMPORT,
        KEY_BUTTON_DRAWABLE_IMPORT + KEY_BUTTON_VIEW_IMPORT,
        parser,
    )
    text = replace_once(text, CONSTANT_MARKER, CONSTANT_MARKER + CONSTANTS, parser)
    text = replace_once(text,
                        "    private int mNavBarMode = NAV_BAR_MODE_3BUTTON;\n",
                        "    private int mNavBarMode = NAV_BAR_MODE_3BUTTON;\n" + FIELD,
                        parser)
    text = replace_once(text, CONSTRUCTOR_MARKER, CONSTRUCTOR_MARKER + CONSTRUCTOR, parser)
    text = replace_once(text, DEFAULT_LAYOUT_MARKER, DEFAULT_LAYOUT, parser)
    text = replace_once(text,
                        "    @Override\n    public void onNavigationModeChanged(int mode) {\n",
                        HELPER + "\n    @Override\n    public void onNavigationModeChanged(int mode) {\n",
                        parser)
    text = install_lifecycle(text, parser)
    text = replace_once(
        text,
        CREATE_VIEW_MARKER,
        CREATE_VIEW_MARKER + CREATE_VIEW + CENTER_CREATE_VIEW,
        parser,
    )
    path.write_text(text)
    print(f"{path}: installed Leaf3 navigation buttons")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
