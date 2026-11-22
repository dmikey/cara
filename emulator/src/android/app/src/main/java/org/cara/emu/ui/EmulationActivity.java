// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu.ui;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.net.Uri;
import android.os.AsyncTask;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.DisplayCutout;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import org.cara.emu.NativeLibrary;
import org.cara.emu.R;
import org.cara.emu.model.GameFile;
import org.cara.emu.settings.Settings;
import org.cara.emu.settings.SettingsFile;
import org.cara.emu.settings.model.Setting;
import org.cara.emu.settings.model.SettingSection;
import org.cara.emu.utils.ControllerMappingHelper;
import org.cara.emu.utils.CaraDirectory;

public final class EmulationActivity extends AppCompatActivity {
    public static final String EXTRA_GAME_ID = "GameId";
    public static final String EXTRA_GAME_NAME = "GameName";
    public static final String EXTRA_GAME_PATH = "GamePath";
    public static final int REQUEST_PICK_IMAGE = 1;
    public static final int REQUEST_CHEAT_CODE = 2;

    private static WeakReference<EmulationActivity> sInstance = new WeakReference<>(null);

    @SuppressLint("StaticFieldLeak")
    private class InitTask extends AsyncTask<Void, Void, Map<Integer, Bitmap>> {
        private final String path = mGamePath;
        private final int launch = mLaunchGeneration;
        private String gameId = mGameId;
        private String gameName = mGameName;

        @Override
        protected Map<Integer, Bitmap> doInBackground(Void... ignored) {
            if (gameId == null || gameId.isEmpty()) {
                gameId = NativeLibrary.GetAppId(path);
                gameName = NativeLibrary.GetAppTitle(path);
            }
            Settings settings = new Settings();
            settings.loadSettings(gameId != null ? gameId : "");
            SettingSection section = settings.getSection(Settings.SECTION_INI_CORE);
            Setting setting = section.getSetting(SettingsFile.KEY_THEME_PACKAGE);
            String theme = setting != null ? setting.getValueAsString() : "";
            return CaraDirectory.loadInputOverlay(EmulationActivity.this, theme);
        }

        @Override
        protected void onPostExecute(Map<Integer, Bitmap> result) {
            if (isDestroyed() || isFinishing() || launch != mLaunchGeneration) {
                return;
            }
            mGameId = gameId;
            mGameName = gameName == null || gameName.isEmpty() ? getString(R.string.app_name) : gameName;
            mBitmaps = result;
            setTitle(mGameName);
            if (mEmulationFragment == null) {
                mEmulationFragment = EmulationFragment.newInstance(path);
                getSupportFragmentManager().beginTransaction()
                        .add(R.id.fragment_emulation, mEmulationFragment)
                        .runOnCommit(EmulationActivity.this::updateBackgroundImage).commit();
            } else {
                refreshControls();
                updateBackgroundImage();
            }
        }
    }

    private String mGameId;
    private String mGameName;
    private String mGamePath;
    private boolean mMenuVisible;
    private int mLaunchGeneration;
    private AlertDialog mSwitchGameDialog;
    private EmulationFragment mEmulationFragment;
    private Map<Integer, Bitmap> mBitmaps;

    private int mSafeInsetLeft = 0;
    private int mSafeInsetTop = 0;
    private int mSafeInsetRight = 0;
    private int mSafeInsetBottom = 0;
    private int mDisplayRotation = 0;
    private float mScaledDensity = 1.0f;

    public static void launch(Context context, GameFile game) {
        Intent intent = new Intent(context, EmulationActivity.class);
        intent.putExtra(EXTRA_GAME_ID, game.getId());
        intent.putExtra(EXTRA_GAME_NAME, game.getName());
        intent.putExtra(EXTRA_GAME_PATH, game.getPath());
        context.startActivity(intent);
    }

    public static EmulationActivity get() {
        return sInstance.get();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_emulation);
        sInstance = new WeakReference<>(this);

        Intent intent = getIntent();
        mGamePath = savedInstanceState == null ? getLaunchPath(intent) :
                savedInstanceState.getString(EXTRA_GAME_PATH);
        if (mGamePath == null) {
            Toast.makeText(this, R.string.game_launch_unavailable, Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        if (savedInstanceState != null) {
            mGameId = savedInstanceState.getString(EXTRA_GAME_ID);
            mGameName = savedInstanceState.getString(EXTRA_GAME_NAME);
        } else if (!Intent.ACTION_VIEW.equals(intent.getAction())) {
            String id = intent.getStringExtra(EXTRA_GAME_ID);
            mGameId = id != null && id.matches("[0-9a-fA-F]{16}") ? id : null;
            mGameName = intent.getStringExtra(EXTRA_GAME_NAME);
        }

        if (!CaraDirectory.isInitialized()) {
            CaraDirectory.start(this);
        }
        mEmulationFragment = (EmulationFragment)getSupportFragmentManager().findFragmentById(
                R.id.fragment_emulation);
        new InitTask().execute();
    }

    private static String getLaunchPath(Intent intent) {
        Uri data = intent.getData();
        if (data == null && intent.getClipData() != null &&
                intent.getClipData().getItemCount() == 1) {
            data = intent.getClipData().getItemAt(0).getUri();
        }
        if (data == null) {
            String path = intent.getStringExtra(EXTRA_GAME_PATH);
            if (path == null || path.isEmpty()) {
                return null;
            }
            if (path.startsWith("/")) {
                // Daijishō's legacy file.path form keeps literal spaces and percent signs.
                data = Uri.fromFile(new File(path));
            } else {
                data = Uri.parse(path);
            }
        }
        if ("content".equals(data.getScheme()) && data.getAuthority() != null) {
            return data.toString();
        }
        if (data.getScheme() != null && !"file".equals(data.getScheme())) {
            return null;
        }
        String path = data.getScheme() == null ? data.toString() : data.getPath();
        if (path == null || !path.startsWith("/")) {
            return null;
        }
        File file = new File(path);
        String name = file.getName().toLowerCase(Locale.ROOT);
        return file.isFile() && file.canRead() &&
                name.matches(".*\\.(3ds|3dsx|cci|cxi|app|axf|elf|cia|zip|7z|z3ds|zcci)") ?
                path : null;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String path = getLaunchPath(intent);
        if (path == null) {
            Toast.makeText(this, R.string.game_launch_unavailable, Toast.LENGTH_LONG).show();
            return;
        }
        if (path.equals(mGamePath)) {
            setIntent(intent);
            return;
        }
        if (mSwitchGameDialog != null) {
            mSwitchGameDialog.dismiss();
        }
        if (mEmulationFragment == null || !mEmulationFragment.hasSession()) {
            replaceGame(intent, path);
        } else if (mEmulationFragment.canSwitchGame()) {
            mSwitchGameDialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.game_launch_switch_title)
                    .setMessage(R.string.game_launch_switch_message)
                    .setNegativeButton(android.R.string.cancel, null)
                    .setPositiveButton(R.string.game_launch_switch_title,
                            (dialog, which) -> mEmulationFragment.stopEmulation(
                                    () -> replaceGame(intent, path)))
                    .create();
            mSwitchGameDialog.setOnDismissListener(dialog -> mSwitchGameDialog = null);
            mSwitchGameDialog.show();
        } else {
            Toast.makeText(this, R.string.game_launch_wait, Toast.LENGTH_SHORT).show();
        }
    }

    private void replaceGame(Intent intent, String path) {
        if (isDestroyed() || isFinishing()) {
            return;
        }
        if (mEmulationFragment != null) {
            getSupportFragmentManager().beginTransaction().remove(mEmulationFragment)
                    .commitNowAllowingStateLoss();
            mEmulationFragment = null;
        }
        setIntent(intent);
        mGamePath = path;
        mGameId = null;
        mGameName = null;
        mLaunchGeneration++;
        new InitTask().execute();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putString(EXTRA_GAME_ID, mGameId);
        outState.putString(EXTRA_GAME_NAME, mGameName);
        outState.putString(EXTRA_GAME_PATH, mGamePath);
        super.onSaveInstanceState(outState);
    }

    @Override
    public void onBackPressed() {
        if (mEmulationFragment == null) {
            finish();
        } else if (mMenuVisible) {
            exitGame();
        } else {
            mMenuVisible = true;
            showRunningSetting();
        }
    }

    public void exitGame() {
        if (mEmulationFragment == null) {
            finish();
        } else {
            mEmulationFragment.stopEmulation(this::finish);
        }
    }

    public void showRunningSetting() {
        mEmulationFragment.stopConfiguringLayout();
        mEmulationFragment.stopConfiguringControls();
        RunningSettingDialog dialog = RunningSettingDialog.newInstance(RunningSettingDialog.MENU_MAIN);
        dialog.show(getSupportFragmentManager(), "RunningSettingDialog");
        dialog.setOnDismissListener(v -> {
            mMenuVisible = false;
            hideSystemUI();
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
    }

    private void hideSystemUI() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        updateDisplayConfig();
    }

    // Gets button presses
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mMenuVisible) {
            return super.dispatchKeyEvent(event);
        }

        InputDevice input = event.getDevice();
        int button = event.getKeyCode();
        int action;
        switch (event.getAction()) {
        case KeyEvent.ACTION_DOWN:
            action = NativeLibrary.ButtonState.PRESSED;
            break;
        case KeyEvent.ACTION_UP:
            action = NativeLibrary.ButtonState.RELEASED;
            break;
        default:
            return super.dispatchKeyEvent(event);
        }

        if (input != null && NativeLibrary.KeyEvent(button, action)) {
            return true;
        } else
            return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (mMenuVisible) {
            return false;
        }

        if (((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0)) {
            return super.dispatchGenericMotionEvent(event);
        }

        // Don't attempt to do anything if we are disconnecting a device.
        if (event.getActionMasked() == MotionEvent.ACTION_CANCEL)
            return true;

        InputDevice input = event.getDevice();
        List<InputDevice.MotionRange> motions = input.getMotionRanges();

        for (InputDevice.MotionRange range : motions) {
            int axis = range.getAxis();
            float origValue = event.getAxisValue(axis);
            float value = ControllerMappingHelper.scaleAxis(input, axis, origValue);
            // If the input is still in the "flat" area, that means it's really zero.
            // This is used to compensate for imprecision in joysticks.
            if (Math.abs(value) > range.getFlat()) {
                NativeLibrary.MoveEvent(axis, value);
            } else {
                NativeLibrary.MoveEvent(axis, 0.0f);
            }
        }

        return true;
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_PICK_IMAGE) {
            if (data == null || data.getData() == null || resultCode != Activity.RESULT_OK) {
                NativeLibrary.HandleImage(new int[0], 0, 0);
                return;
            }

            InputStream inputStream;
            try {
                inputStream = getContentResolver().openInputStream(data.getData());
            } catch (IOException e) {
                NativeLibrary.HandleImage(new int[0], 0, 0);
                return;
            }

            Bitmap image = BitmapFactory.decodeStream(inputStream);
            int srcWidth = image.getWidth();
            int srcHeight = image.getHeight();
            int dstWidth = getIntent().getIntExtra("width", srcWidth);
            int dstHeight = getIntent().getIntExtra("height", srcHeight);
            int[] pixels = new int[dstWidth * dstHeight];
            image = adjustImage(image, dstWidth, dstHeight);
            image.getPixels(pixels, 0, dstWidth, 0, 0, dstWidth, dstHeight);
            NativeLibrary.HandleImage(pixels, dstWidth, dstHeight);
        } else if (requestCode == REQUEST_CHEAT_CODE) {
            NativeLibrary.reloadCheatCode();
        }
    }

    public void updateDisplayConfig() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            DisplayCutout cutout = getWindow().getDecorView().getRootWindowInsets().getDisplayCutout();
            if (cutout != null) {
                mSafeInsetLeft = cutout.getSafeInsetLeft();
                mSafeInsetTop = cutout.getSafeInsetTop();
                mSafeInsetRight = cutout.getSafeInsetRight();
                mSafeInsetBottom = cutout.getSafeInsetBottom();
            }
        }

        mDisplayRotation = getWindowManager().getDefaultDisplay().getRotation();
        mScaledDensity = getResources().getDisplayMetrics().scaledDensity;
    }

    private Bitmap adjustImage(Bitmap image, int width, int height) {
        int srcWidth = image.getWidth();
        int srcHeight = image.getHeight();
        if (width == srcWidth && height == srcHeight) {
            return image;
        }

        int cropX;
        int cropY;
        int cropWidth;
        int cropHeight;

        float ratioNeed = width / (float)height;
        float ratioHave = srcWidth / (float)srcHeight;
        if (ratioNeed > ratioHave) {
            cropWidth = srcWidth;
            cropHeight = (int)(srcWidth / ratioNeed);
            cropX = 0;
            cropY = (srcHeight - cropHeight) / 2;
        } else {
            cropWidth = (int)(srcHeight * ratioNeed);
            cropHeight = srcHeight;
            cropX = (srcWidth - cropWidth) / 2;
            cropY = 0;
        }

        Bitmap cropped = Bitmap.createBitmap(image, cropX, cropY, cropWidth, cropHeight);
        if (cropWidth == width && cropHeight == height) {
            return cropped;
        } else {
            Matrix matrix = new Matrix();
            matrix.postScale(width / (float)cropWidth, height / (float)cropHeight);
            return Bitmap.createBitmap(cropped, 0, 0, cropWidth, cropHeight, matrix, true);
        }
    }

    public void pickImage(int width, int height) {
        Intent intent = getIntent() != null ? getIntent() : new Intent();
        intent.putExtra("width", width);
        intent.putExtra("height", height);
        setIntent(intent);
        //
        intent = new Intent();
        intent.setType("image/*");
        intent.setAction(Intent.ACTION_GET_CONTENT);
        startActivityForResult(Intent.createChooser(intent, "Select Picture"), REQUEST_PICK_IMAGE);
    }

    public void showInputBoxDialog(int maxLength, String error, String hint, String button0, String button1,
                                   String button2) {
        KeyboardDialog.newInstance(maxLength, error, hint, button0, button1, button2)
            .show(getSupportFragmentManager(), "KeyboardDialog");
    }

    public void showMiiSelectorDialog(boolean cancel, String title, String[] miis) {
        MiiSelectorDialog.newInstance(cancel, title, miis)
            .show(getSupportFragmentManager(), "MiiSelectorDialog");
    }

    private AmiiboDialog mAmiiboDialog;
    public void handleNFCScanning(boolean isScanning) {
        if (isScanning) {
            if (mAmiiboDialog == null) {
                mAmiiboDialog = AmiiboDialog.newInstance();
                mAmiiboDialog.show(getSupportFragmentManager(), "AmiiboDialog");
            }
        } else if (mAmiiboDialog != null) {
            mAmiiboDialog.dismiss();
            mAmiiboDialog = null;
        }
    }

    public String getGameName() {
        return mGameName;
    }

    public void refreshControls() {
        mEmulationFragment.refreshControls();
    }

    public void startConfiguringControls() {
        mEmulationFragment.startConfiguringControls();
    }

    public void startConfiguringLayout() {
        mEmulationFragment.startConfiguringLayout();
    }

    public void launchMemoryViewer() {
        MemoryActivity.launch(this);
    }

    public void addNetPlayMessage(String msg) {
        mEmulationFragment.addNetPlayMessage(msg);
    }

    public boolean checkPermission(String permission) {
        return checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    public void updateBackgroundImage() {
        boolean landscape = true;
        int rotation = getDisplayRotation();
        if (rotation == Surface.ROTATION_0 || rotation == Surface.ROTATION_180) {
            landscape = false;
        }
        Bitmap bitmap = mBitmaps.get(landscape ? R.drawable.bg_landscape : R.drawable.bg_portrait);
        if (bitmap != null) {
            bitmap = rotateBitmap(bitmap, 90);
            int width = bitmap.getWidth();
            int height = bitmap.getHeight();
            int[] pixels = new int[width * height];
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
            NativeLibrary.SetBackgroundImage(pixels, width, height);
            bitmap.recycle();
        }
    }

    public Bitmap rotateBitmap(Bitmap source, float angle)  {
        Matrix matrix = new Matrix();
        matrix.postRotate(angle);
        return Bitmap.createBitmap(source, 0, 0, source.getWidth(), source.getHeight(), matrix, true);
    }

    public int getSafeInsetLeft() {
        return mSafeInsetLeft;
    }

    public int getSafeInsetTop() {
        return mSafeInsetTop;
    }

    public int getSafeInsetRight() {
        return mSafeInsetRight;
    }

    public int getSafeInsetBottom() {
        return mSafeInsetBottom;
    }

    public int getDisplayRotation() {
        return mDisplayRotation;
    }

    public float getScaleDensity() {
        return mScaledDensity;
    }

    public void launchCheatCode() {
        Intent intent = new Intent(this, EditorActivity.class);
        intent.putExtra(EditorActivity.EXTRA_GAME_ID, mGameId);
        intent.putExtra(EditorActivity.EXTRA_GAME_NAME, mGameName);
        intent.putExtra(EditorActivity.EXTRA_GAME_PATH, mGamePath);
        startActivityForResult(intent, REQUEST_CHEAT_CODE);
    }

    public void rotateScreen() {
        if (getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        } else {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        }
    }

    public void updateProgress(String name, long written, long total) {
        if (mEmulationFragment != null) {
            mEmulationFragment.updateProgress(name, written, total);
        }
    }

    public Bitmap getInputBitmap(int id, float scale) {
        Bitmap origin = BitmapFactory.decodeResource(getResources(), id);
        float oriWidth = origin.getWidth();
        // theme
        Bitmap bitmap = mBitmaps.get(id);
        int dstWidth = bitmap.getWidth();
        int dstHeight = bitmap.getHeight();
        // adjust scale by default theme bitmap width
        scale *= oriWidth / dstWidth;
        dstWidth = (int)(dstWidth * scale);
        dstHeight = (int)(dstHeight * scale);
        origin.recycle();
        return Bitmap.createScaledBitmap(bitmap, dstWidth, dstHeight, true);
    }
}
