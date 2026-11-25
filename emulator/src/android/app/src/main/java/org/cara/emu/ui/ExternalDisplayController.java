package org.cara.emu.ui;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

import org.cara.emu.NativeLibrary;
import org.cara.emu.settings.Settings;
import org.cara.emu.settings.SettingsFile;
import org.cara.emu.settings.model.Setting;

/** Owns secondary Android windows; EGL ownership stays on the native render thread. */
final class ExternalDisplayController implements DisplayManager.DisplayListener {
    private final Activity activity;
    private final DisplayManager displays;
    private ScreenPresentation presentation;
    private boolean started;
    private int mode;

    ExternalDisplayController(Activity activity) {
        this.activity = activity;
        displays = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
    }

    void start() {
        if (started) return;
        Settings settings = new Settings();
        settings.loadSettings("");
        Setting setting = settings.getSection(Settings.SECTION_INI_RENDERER)
                .getSetting(SettingsFile.KEY_EXTERNAL_DISPLAY);
        mode = 0;
        if (setting != null) {
            try { mode = Integer.parseInt(setting.getValueAsString()); }
            catch (NumberFormatException ignored) { }
        }
        if (mode < 0 || mode > 2) mode = 0;
        started = true;
        displays.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
        update();
    }

    void stop() {
        if (!started) return;
        started = false;
        displays.unregisterDisplayListener(this);
        dismiss();
    }

    private void dismiss() {
        NativeLibrary.ExternalTouchEvent(0, 0, 3);
        NativeLibrary.SetExternalDisplay(null, 0);
        ScreenPresentation old = presentation;
        presentation = null;
        if (old != null) old.dismiss();
    }

    private void update() {
        if (!started || mode == 0 || activity.isFinishing()) {
            dismiss();
            return;
        }
        Display selected = null;
        for (Display display : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display.isValid() && display.getDisplayId() != activity.getWindowManager()
                    .getDefaultDisplay().getDisplayId()) {
                if (presentation != null && presentation.getDisplay().getDisplayId()
                        == display.getDisplayId()) {
                    selected = display;
                    break;
                }
                if (selected == null) selected = display;
            }
        }
        if (selected == null) { dismiss(); return; }
        if (presentation != null && presentation.getDisplay().getDisplayId()
                == selected.getDisplayId()) return;
        dismiss();
        ScreenPresentation next = new ScreenPresentation(activity, selected);
        presentation = next;
        next.setOnDismissListener(dialog -> {
            if (presentation == next) {
                presentation = null;
                NativeLibrary.SetExternalDisplay(null, 0);
            }
        });
        try { next.show(); }
        catch (WindowManager.InvalidDisplayException e) { dismiss(); }
    }

    @Override public void onDisplayAdded(int id) { update(); }
    @Override public void onDisplayRemoved(int id) { update(); }
    @Override public void onDisplayChanged(int id) { update(); }

    private final class ScreenPresentation extends Presentation implements SurfaceHolder.Callback {
        private int touchPointer = -1;
        ScreenPresentation(Context context, Display display) { super(context, display); }

        @Override protected void onCreate(Bundle state) {
            super.onCreate(state);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            SurfaceView view = new SurfaceView(getContext());
            view.getHolder().addCallback(this);
            view.setOnTouchListener((v, event) -> {
                if (mode != 2 || presentation != this) return false;
                int action = event.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN) {
                    touchPointer = event.getPointerId(0);
                } else if (action == MotionEvent.ACTION_CANCEL) {
                    NativeLibrary.ExternalTouchEvent(0, 0, 3);
                    touchPointer = -1;
                    return true;
                } else if (action == MotionEvent.ACTION_POINTER_UP) {
                    if (event.getPointerId(event.getActionIndex()) == touchPointer) {
                        NativeLibrary.ExternalTouchEvent(0, 0, 1);
                        touchPointer = -1;
                    }
                    return true;
                } else if (action != MotionEvent.ACTION_MOVE && action != MotionEvent.ACTION_UP) {
                    return true;
                }
                int index = event.findPointerIndex(touchPointer);
                if (index < 0 || v.getWidth() <= 0 || v.getHeight() <= 0) return true;
                NativeLibrary.ExternalTouchEvent(event.getX(index) / v.getWidth(),
                        event.getY(index) / v.getHeight(), action);
                if (action == MotionEvent.ACTION_UP) touchPointer = -1;
                return true;
            });
            setContentView(view);
        }

        @Override public void surfaceCreated(SurfaceHolder holder) { publish(holder); }
        @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            publish(holder);
        }
        private void publish(SurfaceHolder holder) {
            if (started && presentation == this && holder.getSurface().isValid())
                NativeLibrary.SetExternalDisplay(holder.getSurface(), mode);
        }
        @Override public void surfaceDestroyed(SurfaceHolder holder) {
            if (presentation == this) NativeLibrary.SetExternalDisplay(null, 0);
        }
    }
}
