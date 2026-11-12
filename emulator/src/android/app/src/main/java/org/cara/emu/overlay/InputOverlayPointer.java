// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.overlay;

import org.cara.emu.NativeLibrary;

public final class InputOverlayPointer {
    private int mPointerId;

    public InputOverlayPointer() {
        mPointerId = -1;
    }

    public void onPointerDown(int id, float x, float y) {
        mPointerId = id;
        int action = NativeLibrary.TouchEvent.TOUCH_PRESSED;
        NativeLibrary.TouchEvent(action, (int)x, (int)y);
    }

    public void onPointerMove(int id, float x, float y) {
        int action = NativeLibrary.TouchEvent.TOUCH_MOVED;
        NativeLibrary.TouchEvent(action, (int)x, (int)y);
    }

    public void onPointerUp(int id, float x, float y) {
        mPointerId = -1;
        int action = NativeLibrary.TouchEvent.TOUCH_RELEASED;
        NativeLibrary.TouchEvent(action, (int)x, (int)y);
    }

    public int getPointerId() {
        return mPointerId;
    }
}
