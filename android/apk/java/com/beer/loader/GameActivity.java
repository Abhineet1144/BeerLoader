package com.beer.loader;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.PopupWindow;

/**
 * The game, with an optional on-screen Xbox controller ({@link PadView}) in a transparent window over it. Started by the
 * start screen; for tests `am start -n com.beer.loader/.GameActivity --ez beer.pad true` reads launch.txt like the plain
 * NativeActivity does.
 */
public class GameActivity extends NativeActivity {
    static {
        System.loadLibrary("beer");   // the same library NativeActivity runs; here so PadView's native methods resolve
    }

    static final String PAD_EXTRA = "beer.pad";
    PopupWindow popup;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused && popup == null && getIntent().getBooleanExtra(PAD_EXTRA, false)) {
            PadView pad = new PadView(this);
            popup = new PopupWindow(pad, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            popup.setTouchable(true);
            popup.setClippingEnabled(false);
            popup.showAtLocation(getWindow().getDecorView(), Gravity.NO_GRAVITY, 0, 0);
        }
    }

    @Override
    protected void onDestroy() {
        if (popup != null) popup.dismiss();
        super.onDestroy();
    }
}
