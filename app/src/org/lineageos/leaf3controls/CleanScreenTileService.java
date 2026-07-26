package org.lineageos.leaf3controls;

import android.os.SystemClock;
import android.os.SystemProperties;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public final class CleanScreenTileService extends TileService {
  @Override
  public void onStartListening() {
    updateTile();
  }

  @Override
  public void onClick() {
    SystemProperties.set(Leaf3Settings.FULL_REFRESH,
                         Long.toString(SystemClock.elapsedRealtimeNanos()));
    updateTile();
  }

  private void updateTile() {
    final Tile tile = getQsTile();
    if (tile == null) {
      return;
    }
    tile.setState(Tile.STATE_INACTIVE);
    tile.setLabel(getString(R.string.clean_tile_label));
    tile.setContentDescription(tile.getLabel());
    tile.updateTile();
  }
}
