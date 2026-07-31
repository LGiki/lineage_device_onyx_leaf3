package org.lineageos.leaf3controls;

import android.content.Intent;
import android.os.SystemProperties;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public final class RefreshModeTileService extends TileService {
  @Override
  public void onStartListening() {
    updateTile(effectiveMode());
  }

  @Override
  public void onClick() {
    final String nextMode = Leaf3Settings.nextMode(effectiveMode());
    final Intent intent = new Intent(this, Leaf3StateService.class);
    intent.setAction(Leaf3StateService.ACTION_TEMPORARY_MODE);
    intent.putExtra(Leaf3StateService.EXTRA_REFRESH_MODE, nextMode);
    startService(intent);
  }

  private String effectiveMode() {
    final String active =
        SystemProperties.get(Leaf3Settings.ACTIVE_REFRESH_MODE, "");
    if (Leaf3Settings.isRefreshMode(active)) {
      return active;
    }
    final String global =
        SystemProperties.get(Leaf3Settings.GLOBAL_REFRESH_MODE,
                             Leaf3Settings.MODE_BALANCED);
    return Leaf3Settings.isRefreshMode(global)
        ? global : Leaf3Settings.MODE_BALANCED;
  }

  private void updateTile(String mode) {
    final Tile tile = getQsTile();
    if (tile == null) {
      return;
    }
    tile.setState(Tile.STATE_ACTIVE);
    tile.setLabel(getString(R.string.refresh_tile_label,
                            Leaf3Settings.modeLabel(this, mode)));
    tile.setContentDescription(tile.getLabel());
    tile.updateTile();
  }
}
