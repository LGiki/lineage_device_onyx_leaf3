#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Add Leaf3's optional full-refresh key to Android 11's navigation bar."""

import argparse
from pathlib import Path


SYSTEM_CLOCK_IMPORT = "import android.os.SystemClock;\n"
SYSTEM_PROPERTIES_IMPORT = "import android.os.SystemProperties;\n"
IMPORT_MARKER = "import android.graphics.drawable.Icon;\n"
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
    "    private static final String LEAF3_NAV_REFRESH_BUTTON =\n"
    "            \"persist.sys.leaf3.nav_refresh_button\";\n"
    "    private static final String LEAF3_FULL_REFRESH = \"sys.leaf3.full_refresh\";\n"
)
FIELD = "    private boolean mLeaf3RefreshButtonEnabled;\n"
CONSTRUCTOR = (
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
)
CREATE_VIEW = (
    "        } else if (LEAF3_REFRESH.equals(button)) {\n"
    "            v = inflater.inflate(R.layout.leaf3_refresh, parent, false);\n"
    "            v.setOnClickListener(view -> SystemProperties.set(LEAF3_FULL_REFRESH,\n"
    "                    Long.toString(SystemClock.elapsedRealtimeNanos())));\n"
)


def replace_once(text: str, old: str, new: str, parser: argparse.ArgumentParser) -> str:
    if text.count(old) != 1:
        parser.error(f"expected exactly one occurrence of: {old.strip()}")
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("navigation_bar_inflater", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    path = args.navigation_bar_inflater
    text = path.read_text()
    if "LEAF3_NAV_REFRESH_BUTTON" in text:
        required = (
            "import android.os.SystemClock;",
            "import android.os.SystemProperties;",
            "public static final String LEAF3_REFRESH = \"leaf3_refresh\";",
            "private boolean mLeaf3RefreshButtonEnabled;",
            "R.string.config_navBarLayoutLeaf3Refresh",
            "R.layout.leaf3_refresh",
            "SystemProperties.set(LEAF3_FULL_REFRESH,",
        )
        if not all(marker in text for marker in required):
            parser.error(f"{path}: incomplete Leaf3 refresh-button patch")
        print(f"{path}: Leaf3 refresh button is installed")
        return 0

    if args.check:
        parser.error(f"{path}: Leaf3 refresh button is not installed")

    text = replace_once(text, IMPORT_MARKER,
                        IMPORT_MARKER + SYSTEM_CLOCK_IMPORT + SYSTEM_PROPERTIES_IMPORT,
                        parser)
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
    text = replace_once(text, CREATE_VIEW_MARKER, CREATE_VIEW_MARKER + CREATE_VIEW, parser)
    path.write_text(text)
    print(f"{path}: installed Leaf3 refresh button")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
