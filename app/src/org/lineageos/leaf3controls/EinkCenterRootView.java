package org.lineageos.leaf3controls;

import android.content.Context;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.widget.FrameLayout;

public final class EinkCenterRootView extends FrameLayout {
  private Runnable dismissAction;

  public EinkCenterRootView(Context context, AttributeSet attributes) {
    super(context, attributes);
  }

  void setDismissAction(Runnable action) {
    dismissAction = action;
  }

  @Override
  public boolean dispatchKeyEvent(KeyEvent event) {
    if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
      if (event.getAction() == KeyEvent.ACTION_UP && dismissAction != null) {
        dismissAction.run();
      }
      return true;
    }
    return super.dispatchKeyEvent(event);
  }
}
