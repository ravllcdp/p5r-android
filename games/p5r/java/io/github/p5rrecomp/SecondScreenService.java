package io.github.p5rrecomp;

import android.app.Presentation;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Typeface;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Display;
import android.view.View;

import java.io.File;
import java.io.FileInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Displays live party HP/SP data on the AYN Thor's second screen (Display #4,
 * FLAG_PRESENTATION, 1080×1240 landscape). Visual design matches the Eden Duo
 * companion for P5R exactly: parallelogram party cards, game portrait sprites,
 * red/cyan/magenta colour scheme.
 *
 * Sprites are extracted from the game's own RomFS by second_screen_assets.cpp
 * (C++) and written to {data_root}/companion/ as raw RGBA files on first run.
 *
 * Back-gesture fix: setCancelable(false) + empty onBackPressed() prevent the
 * system Dialog back-dismiss that closed the Presentation on the AYN Thor.
 *
 * Polls JNI at 10 Hz; started by Launcher before NativeActivity.
 */
public class SecondScreenService extends Service {

    static {
        try { System.loadLibrary("p5r"); } catch (UnsatisfiedLinkError ignored) {}
    }

    // JNI: returns int[34] — see second_screen_jni.cpp for layout.
    native int[] nativeGetSnapshot();

    // -----------------------------------------------------------------------
    // Eden Duo colour palette (from manifest.json)
    // -----------------------------------------------------------------------
    private static final int COL_BG       = 0xFF100C0E;
    private static final int COL_CARD     = 0xFF221A1E;
    private static final int COL_RED      = 0xFFE5191C;
    private static final int COL_HP       = 0xFF66FDDC;
    private static final int COL_SP       = 0xFFFF64E8;
    private static final int COL_HP_BG    = 0xFF1F3F3A;
    private static final int COL_SP_BG    = 0xFF3F1F3A;
    private static final int COL_WHITE    = 0xFFFFFFFF;
    private static final int COL_GREY     = 0xFF888888;
    private static final int COL_DIM      = 0x40FFFFFF;

    // -----------------------------------------------------------------------
    // Party member names (kUnitMemberId index)
    // -----------------------------------------------------------------------
    private static final String[] NAMES = {
        "Joker", "Ryuji", "Morgana", "Ann", "Yusuke",
        "Makoto", "Futaba", "Haru", "Akechi", "Kasumi", "Lavenza"
    };

    // -----------------------------------------------------------------------
    // Canvas layout constants (canvas = 1240×1080, landscape)
    // Per-slot card: 290×130 parallelogram. The 4 cards sit in a row at y=821.
    // Card x offsets: slot 0 at x=30, then every 305 px.
    // Parallelogram: (10,0)→(290,0)→(280,130)→(0,130), skew=10px
    // -----------------------------------------------------------------------
    private static final int CANVAS_W = 1240;
    private static final int CANVAS_H = 1080;

    // Card geometry in card-local coordinates
    private static final int CARD_W  = 290;
    private static final int CARD_H  = 130;
    private static final int CARD_SKEW = 10;   // parallelogram tilt

    // Position of the first card (top-left of its bounding box in canvas coords)
    private static final int CARDS_Y    = 821;
    private static final int CARDS_X0   = 30;
    private static final int CARD_STEP  = 305;

    // Portrait thumbnail in the card: 52×84, left-aligned, vertically centred
    private static final int PORT_W = 52;
    private static final int PORT_H = 84;
    private static final int PORT_X = 12;  // in card coords
    private static final int PORT_Y = (CARD_H - PORT_H) / 2;

    // HP/SP bar geometry (in card coords, right of portrait)
    private static final int BAR_X     = PORT_X + PORT_W + 8;
    private static final int BAR_W     = 180;
    private static final int BAR_H     = 8;
    private static final int HP_BAR_Y  = 42;
    private static final int SP_BAR_Y  = 70;
    private static final int ICON_W    = 20;
    private static final int ICON_H    = 14;
    private static final int LV_X      = BAR_X;
    private static final int LV_Y      = 20;

    // -----------------------------------------------------------------------
    private Presentation   mPresentation;
    private CompanionView  mView;
    private final Handler  mHandler = new Handler(Looper.getMainLooper());
    private boolean        mRunning;

    // Latest snapshot decoded from JNI
    private int[]  mSnap;

    // Loaded portraits (index = member id, null = not loaded yet)
    private final Bitmap[]  mPortraits = new Bitmap[11];
    private Bitmap          mIconHp;
    private Bitmap          mIconSp;
    private boolean         mAssetsLoaded;

    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            if (!mRunning) return;
            tick();
            mHandler.postDelayed(this, 100);
        }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (mRunning) return START_STICKY;
        mRunning = true;

        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display[] all = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (all != null && all.length > 0) {
            GamePresentation p = new GamePresentation(this, all[all.length - 1]);
            p.setCancelable(false);
            p.show();
            mPresentation = p;
        }

        mHandler.post(mPoll);
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mRunning = false;
        mHandler.removeCallbacks(mPoll);
        if (mPresentation != null) { mPresentation.dismiss(); mPresentation = null; }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    // Data root where second_screen_assets.cpp writes companion/*.rgba files.
    // Mirrors plat::data_root(): "/sdcard/Persona 5 Royal" on Android.
    private String getDataRoot() {
        return android.os.Environment.getExternalStorageDirectory() + "/Persona 5 Royal";
    }

    // Try to load extracted assets; called lazily on the first tick.
    private void loadAssets() {
        if (mAssetsLoaded) return;
        mAssetsLoaded = true;
        String root = getDataRoot() + "/companion";
        for (int id = 0; id < mPortraits.length; id++) {
            mPortraits[id] = loadRgba(root + "/portraits/p" + id + ".rgba");
        }
        mIconHp = loadRgba(root + "/icons/hp.rgba");
        mIconSp = loadRgba(root + "/icons/sp.rgba");
    }

    // Load a raw RGBA file written by second_screen_assets.cpp.
    private static Bitmap loadRgba(String path) {
        try {
            File f = new File(path);
            if (!f.exists()) return null;
            byte[] data = new byte[(int) f.length()];
            try (FileInputStream fis = new FileInputStream(f)) {
                int off = 0, rem = data.length, r;
                while (rem > 0 && (r = fis.read(data, off, rem)) > 0) { off += r; rem -= r; }
            }
            if (data.length < 8) return null;
            ByteBuffer bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            int w = bb.getInt(), h = bb.getInt();
            if (w <= 0 || h <= 0 || data.length < 8 + w * h * 4) return null;
            int[] pixels = new int[w * h];
            for (int i = 0; i < w * h; i++) {
                int r2 = bb.get() & 0xFF, g = bb.get() & 0xFF,
                    b  = bb.get() & 0xFF, a = bb.get() & 0xFF;
                pixels[i] = (a << 24) | (r2 << 16) | (g << 8) | b;
            }
            return Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888);
        } catch (Exception e) {
            return null;
        }
    }

    private void tick() {
        if (mPresentation == null || !mPresentation.isShowing() || mView == null) return;
        loadAssets();
        try { mSnap = nativeGetSnapshot(); } catch (Throwable t) { mSnap = null; }
        mView.postInvalidate();
    }

    // -----------------------------------------------------------------------
    // Custom Canvas view — the whole companion UI
    // -----------------------------------------------------------------------

    private class CompanionView extends View {
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        CompanionView(Context ctx) { super(ctx); }

        @Override
        protected void onDraw(Canvas canvas) {
            // Scale from logical 1240×1080 to actual view size
            float scaleX = getWidth()  / (float) CANVAS_W;
            float scaleY = getHeight() / (float) CANVAS_H;
            canvas.scale(scaleX, scaleY);

            // Background
            canvas.drawColor(COL_BG);

            // Header bar — thin red line across the top
            mPaint.setStyle(Paint.Style.FILL);
            mPaint.setColor(COL_RED);
            canvas.drawRect(0, 0, CANVAS_W, 4, mPaint);

            // "PERSONA 5 ROYAL" title text
            mPaint.setColor(COL_RED);
            mPaint.setTextSize(28f);
            mPaint.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
            canvas.drawText("PERSONA 5 ROYAL", 30, 48, mPaint);

            int[] snap = mSnap;
            boolean valid = snap != null && snap.length >= 34 && snap[0] == 1;

            for (int slot = 0; slot < 4; slot++) {
                int cardLeft = CARDS_X0 + slot * CARD_STEP;
                boolean on = false;
                int id = 0, hp = 0, hpMax = 1, sp = 0, spMax = 1, lv = 1;
                if (valid) {
                    int b = 2 + slot * 8;
                    on    = snap[b]     == 1;
                    id    = snap[b + 1];
                    hp    = snap[b + 2]; hpMax = snap[b + 3];
                    sp    = snap[b + 4]; spMax = snap[b + 5];
                    lv    = snap[b + 6];
                }
                drawCard(canvas, cardLeft, CARDS_Y, on, id, hp, hpMax, sp, spMax, lv);
            }
        }

        // Draw a single party card at canvas coordinates (left, top).
        private void drawCard(Canvas canvas, int left, int top,
                              boolean on, int memberId,
                              int hp, int hpMax, int sp, int spMax, int lv) {

            canvas.save();
            canvas.translate(left, top);

            // --- Parallelogram clip & fill ---
            // Vertices: top-left skewed right by CARD_SKEW
            Path path = new Path();
            path.moveTo(CARD_SKEW,  0);
            path.lineTo(CARD_W,     0);
            path.lineTo(CARD_W - CARD_SKEW, CARD_H);
            path.lineTo(0,          CARD_H);
            path.close();

            mPaint.setStyle(Paint.Style.FILL);
            mPaint.setColor(on ? COL_CARD : Color.argb(0x40, 0x22, 0x1A, 0x1E));
            canvas.drawPath(path, mPaint);

            // Red accent line at top
            mPaint.setStyle(Paint.Style.STROKE);
            mPaint.setStrokeWidth(3f);
            mPaint.setColor(COL_RED);
            canvas.drawLine(CARD_SKEW, 1.5f, CARD_W, 1.5f, mPaint);
            mPaint.setStyle(Paint.Style.FILL);

            if (!on) {
                // Dim dash in empty slot
                mPaint.setColor(COL_DIM);
                mPaint.setTextSize(32f);
                mPaint.setTypeface(Typeface.DEFAULT_BOLD);
                canvas.drawText("—", CARD_W / 2f - 10, CARD_H / 2f + 10, mPaint);
                canvas.restore();
                return;
            }

            // --- Portrait ---
            Bitmap portrait = (memberId >= 0 && memberId < mPortraits.length)
                              ? mPortraits[memberId] : null;
            if (portrait != null) {
                android.graphics.Rect src = new android.graphics.Rect(0, 0, portrait.getWidth(), portrait.getHeight());
                android.graphics.Rect dst = new android.graphics.Rect(PORT_X, PORT_Y, PORT_X + PORT_W, PORT_Y + PORT_H);
                canvas.drawBitmap(portrait, src, dst, null);
            } else {
                // Fallback: coloured circle
                mPaint.setColor(COL_RED);
                canvas.drawCircle(PORT_X + PORT_W / 2f, PORT_Y + PORT_H / 2f,
                                  Math.min(PORT_W, PORT_H) / 2f, mPaint);
            }

            // --- Name + Level ---
            String name = (memberId >= 0 && memberId < NAMES.length) ? NAMES[memberId] : "?";
            mPaint.setColor(COL_WHITE);
            mPaint.setTextSize(16f);
            mPaint.setTypeface(Typeface.DEFAULT_BOLD);
            canvas.drawText("Lv." + lv, LV_X, LV_Y, mPaint);
            mPaint.setTextSize(18f);
            canvas.drawText(name, LV_X + 40, LV_Y, mPaint);

            // --- HP bar ---
            drawBar(canvas, BAR_X, HP_BAR_Y, BAR_W, BAR_H,
                    hp, hpMax, COL_HP, COL_HP_BG, mIconHp,
                    String.valueOf(hp));

            // --- SP bar ---
            drawBar(canvas, BAR_X, SP_BAR_Y, BAR_W, BAR_H,
                    sp, spMax, COL_SP, COL_SP_BG, mIconSp,
                    String.valueOf(sp));

            canvas.restore();
        }

        // Draw a stat bar with optional icon to its left and value text to its right.
        private void drawBar(Canvas canvas, int x, int y, int w, int h,
                             int cur, int max, int colFill, int colBg,
                             Bitmap icon, String valStr) {
            // Icon
            if (icon != null) {
                android.graphics.Rect dst = new android.graphics.Rect(
                    x - ICON_W - 4, y - 2, x - 4, y + ICON_H);
                canvas.drawBitmap(icon, null, dst, null);
            }

            // Background track
            mPaint.setColor(colBg);
            canvas.drawRect(x, y, x + w, y + h, mPaint);

            // Fill
            float fill = max > 0 ? Math.max(0f, Math.min(1f, cur / (float) max)) : 0f;
            mPaint.setColor(colFill);
            canvas.drawRect(x, y, x + w * fill, y + h, mPaint);

            // Value text
            mPaint.setColor(COL_WHITE);
            mPaint.setTextSize(14f);
            mPaint.setTypeface(Typeface.DEFAULT);
            canvas.drawText(valStr, x + w + 6, y + h + 1, mPaint);
        }
    }

    // -----------------------------------------------------------------------
    // Presentation — renders on the second physical screen
    // -----------------------------------------------------------------------

    private class GamePresentation extends Presentation {
        GamePresentation(Context ctx, Display d) { super(ctx, d); }

        @Override
        public void onCreate(android.os.Bundle b) {
            super.onCreate(b);
            mView = new CompanionView(getContext());
            mView.setBackgroundColor(COL_BG);
            setContentView(mView);
        }

        // Prevent the system back gesture from dismissing the Presentation.
        @Override
        public void onBackPressed() { /* do nothing */ }
    }
}
