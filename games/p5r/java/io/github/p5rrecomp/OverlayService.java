package io.github.p5rrecomp;

import android.app.Service;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * In-game settings, hidden until needed (Persona 5 look: red / black / white, slanted shapes).
 *
 *  1. listenView - a 1x1 px overlay that receives ACTION_OUTSIDE for every touch that lands on the game. It never
 *                  takes a touch away from the game. Nothing is drawn.
 *  2. iconView   - a small slanted red badge in the top-right corner. Fades in when the screen is touched and fades
 *                  out on its own after ICON_SECONDS. Only exists as a window while visible, so it never blocks the
 *                  game's own touch controls.
 *  3. panelView  - the settings panel, opened by tapping the badge. The game keeps running behind it.
 *
 * Most settings need a game restart to take effect; "Guardar" shows a toast about this.
 * Started by Launcher just before NativeActivity; lives for the life of the game process.
 */
public class OverlayService extends Service {

    private static final long ICON_MS = 3000;
    private static final int RED = Color.rgb(230, 0, 18);
    private static final int DARK_RED = Color.rgb(140, 0, 10);
    private static final int BLACK = Color.rgb(10, 10, 10);

    // ---- option definitions (subset most useful during play) ----
    private static final class Opt {
        final String key, label;
        final String[] vals, valLabels;
        int pick;
        Opt(String key, String label, String[] vals, String[] valLabels, int def) {
            this.key = key; this.label = label;
            this.vals = vals; this.valLabels = valLabels; this.pick = def;
        }
    }

    private final List<Opt> mOpts = new ArrayList<>();
    private final Map<String, String> mCfg = new LinkedHashMap<>();
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private WindowManager mWm;
    private View mListenView;
    private View mIconView;
    private View mPanelRoot;
    private boolean mIconShown;
    private boolean mPanelOpen;

    private final Runnable mHideIcon = new Runnable() {
        @Override public void run() { hideIcon(); }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (mWm != null) return START_STICKY; // already running
        mWm = (WindowManager) getSystemService(WINDOW_SERVICE);
        buildOptions();
        addListenView();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mHideIcon);
        if (mWm != null) {
            removeQuiet(mListenView);
            removeQuiet(mIconView);
            removeQuiet(mPanelRoot);
        }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent i) { return null; }

    private void removeQuiet(View v) {
        if (v != null && v.getWindowToken() != null) {
            try { mWm.removeView(v); } catch (Exception ignored) { }
        }
    }

    // -----------------------------------------------------------------------
    // Options and config
    // -----------------------------------------------------------------------

    private void buildOptions() {
        mOpts.clear();
        mOpts.add(new Opt("NX_FPS_CAP",    "FPS",
                new String[]{"30","60"}, new String[]{"30 fps","60 fps"}, 0));
        mOpts.add(new Opt("RESOLUTION",    "Resolución",
                new String[]{"360p","480p","720p","1080p"}, new String[]{"360p","480p","720p","1080p"}, 2));
        mOpts.add(new Opt("GRAPHICS",      "Gráficos",
                new String[]{"low","medium","high"}, new String[]{"Bajos","Medios","Altos"}, 0));
        mOpts.add(new Opt("NX_SHADOWS",    "Sombras",
                new String[]{"0","1"}, new String[]{"Desactivadas","Activadas"}, 0));
        mOpts.add(new Opt("NX_TOUCH_SHOW", "Controles táctiles",
                new String[]{"1","0"}, new String[]{"Activados","Desactivados"}, 0));
        mOpts.add(new Opt("NX_SHOW_FPS",   "Contador FPS",
                new String[]{"0","1"}, new String[]{"Desactivado","Activado"}, 0));
        loadConfig();
    }

    private File configFile() {
        return new File(android.os.Environment.getExternalStorageDirectory(),
                "Persona 5 Royal/config.txt");
    }

    private void loadConfig() {
        mCfg.clear();
        mCfg.putAll(Launcher.readConfig(configFile()));
        for (Opt o : mOpts) {
            String v = "RESOLUTION".equals(o.key) ? resolveResolution() : mCfg.get(o.key);
            if (v != null) {
                for (int i = 0; i < o.vals.length; i++) {
                    if (o.vals[i].equals(v)) { o.pick = i; break; }
                }
            }
        }
    }

    private String resolveResolution() {
        String r = mCfg.get("RESOLUTION");
        if (r != null) return r;
        String hh = mCfg.get("NX_HANDHELD");
        if ("0".equals(hh)) return "1080p";
        String res = mCfg.get("resolution");
        if (res != null && res.contains("360")) return "360p";
        if (res != null && res.contains("480")) return "480p";
        return "720p";
    }

    private void saveConfig() {
        mCfg.putAll(Launcher.readConfig(configFile()));
        for (Opt o : mOpts) mCfg.put(o.key, o.vals[o.pick]);
        // Resolution aliases
        String res = mCfg.get("RESOLUTION");
        if (res != null) {
            mCfg.put("NX_HANDHELD",   "1080p".equals(res) ? "0" : "1");
            mCfg.put("resolution",    res);
            mCfg.put("NX_RENDER_RES", res);
            mCfg.put("NX_RES",        res);
        }
        String gfx = mCfg.get("GRAPHICS");
        mCfg.remove("NX_LOD_BIAS");
        if ("low".equals(gfx))    mCfg.put("NX_LOD_BIAS", "1");
        else if ("medium".equals(gfx)) mCfg.put("NX_LOD_BIAS", "0.5");
        Launcher.writeConfig(configFile(), mCfg);
    }

    // -----------------------------------------------------------------------
    // Touch listener (1x1, invisible, takes no touches)
    // -----------------------------------------------------------------------

    private void addListenView() {
        View v = new View(this);
        v.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View view, MotionEvent ev) {
                // ACTION_OUTSIDE: a touch landed somewhere else on screen (the game). One event per touch.
                if (ev.getActionMasked() == MotionEvent.ACTION_OUTSIDE && !mPanelOpen) showIcon();
                return false;
            }
        });
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                  | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                1, 1, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.TOP | Gravity.START;
        mListenView = v;
        mWm.addView(mListenView, p);
    }

    // -----------------------------------------------------------------------
    // Settings badge (appears on touch, hides itself)
    // -----------------------------------------------------------------------

    private void showIcon() {
        mHandler.removeCallbacks(mHideIcon);
        mHandler.postDelayed(mHideIcon, ICON_MS);
        if (mIconShown) return;
        mIconShown = true;

        View badge = new BadgeView(this);
        badge.setAlpha(0f);
        badge.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                mHandler.removeCallbacks(mHideIcon);
                removeIconNow();
                openPanel();
            }
        });
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                  | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                dp(64), dp(52), WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.TOP | Gravity.END;
        p.x = dp(10);
        p.y = dp(10);
        mIconView = badge;
        mWm.addView(mIconView, p);
        badge.animate().alpha(1f).setDuration(150).start();
    }

    private void hideIcon() {
        if (!mIconShown || mIconView == null) return;
        final View badge = mIconView;
        badge.animate().alpha(0f).setDuration(200).withEndAction(new Runnable() {
            @Override public void run() {
                if (mIconView == badge) removeIconNow();
            }
        }).start();
    }

    private void removeIconNow() {
        mIconShown = false;
        if (mIconView != null) {
            mIconView.animate().cancel();
            removeQuiet(mIconView);
            mIconView = null;
        }
    }

    // -----------------------------------------------------------------------
    // Settings panel
    // -----------------------------------------------------------------------

    private WindowManager.LayoutParams panelParams() {
        int flags = WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                dp(380), WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.END | Gravity.TOP;
        return p;
    }

    private void openPanel() {
        if (mPanelOpen) return;
        mPanelOpen = true;

        loadConfig(); // refresh values from disk

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(new SlantDrawable(BLACK, Color.TRANSPARENT, 0, dp(36), true));
        root.setPadding(dp(40), 0, 0, 0);

        // Header: red slab, bold italic title, black close badge
        FrameLayout header = new FrameLayout(this);
        header.setBackground(new SlantDrawable(RED, Color.TRANSPARENT, 0, dp(18), false));
        header.setPadding(dp(18), dp(10), dp(14), dp(10));

        TextView title = new TextView(this);
        title.setText("AJUSTES");
        title.setTextColor(Color.WHITE);
        title.setTextSize(24);
        title.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        title.setLetterSpacing(0.08f);
        header.addView(title, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_VERTICAL | Gravity.START));

        TextView close = slantButton("X", BLACK, Color.rgb(60, 60, 60), Color.WHITE, 16, dp(42), dp(34));
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { closePanel(); }
        });
        header.addView(close, new FrameLayout.LayoutParams(dp(42), dp(34), Gravity.CENTER_VERTICAL | Gravity.END));
        root.addView(header, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // Note
        TextView note = new TextView(this);
        note.setText("Los cambios se aplican al reiniciar el juego.");
        note.setTextColor(Color.rgb(200, 200, 200));
        note.setTextSize(12);
        note.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.ITALIC));
        note.setPadding(dp(18), dp(8), dp(16), dp(2));
        root.addView(note);

        // Options
        ScrollView scroll = new ScrollView(this);
        LinearLayout optList = new LinearLayout(this);
        optList.setOrientation(LinearLayout.VERTICAL);
        optList.setPadding(dp(18), dp(2), dp(16), dp(4));

        for (final Opt o : mOpts) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(7), 0, dp(7));

            TextView lbl = new TextView(this);
            lbl.setText(o.label.toUpperCase());
            lbl.setTextColor(Color.WHITE);
            lbl.setTextSize(15);
            lbl.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
            row.addView(lbl, new LinearLayout.LayoutParams(0,
                    LinearLayout.LayoutParams.WRAP_CONTENT, 1));

            final TextView btn = slantButton(o.valLabels[o.pick], RED, DARK_RED, Color.WHITE, 14, dp(128), dp(38));
            btn.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    o.pick = (o.pick + 1) % o.vals.length;
                    btn.setText(o.valLabels[o.pick]);
                }
            });
            row.addView(btn, new LinearLayout.LayoutParams(dp(128), dp(38)));
            optList.addView(row);

            View sep = new View(this);
            sep.setBackground(new DiagonalLineDrawable(Color.argb(120, 255, 255, 255)));
            optList.addView(sep, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(6)));
        }

        scroll.addView(optList);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        // Save button: white slab with red text, inverts on press
        TextView save = slantButton("GUARDAR", Color.WHITE, Color.rgb(200, 200, 200), RED, 18, 0, dp(52));
        save.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                saveConfig();
                android.widget.Toast.makeText(OverlayService.this,
                        "Guardado. Reinicia el juego para aplicar.",
                        android.widget.Toast.LENGTH_LONG).show();
                closePanel();
            }
        });
        LinearLayout.LayoutParams saveLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        saveLp.setMargins(dp(18), dp(8), dp(16), dp(16));
        root.addView(save, saveLp);

        mPanelRoot = root;
        mWm.addView(mPanelRoot, panelParams());
    }

    private void closePanel() {
        if (!mPanelOpen) return;
        mPanelOpen = false;
        removeQuiet(mPanelRoot);
        mPanelRoot = null;
    }

    // -----------------------------------------------------------------------
    // Look: everything is drawn in code, no image assets
    // -----------------------------------------------------------------------

    private TextView slantButton(String text, int fill, int pressedFill, int textColor, int sp, int w, int h) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(textColor);
        t.setTextSize(sp);
        t.setGravity(Gravity.CENTER);
        t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        t.setClickable(true);
        StateListDrawable sl = new StateListDrawable();
        sl.addState(new int[]{android.R.attr.state_pressed},
                new SlantDrawable(pressedFill, Color.WHITE, dp(2), dp(10), false));
        sl.addState(new int[]{},
                new SlantDrawable(fill, Color.WHITE, dp(2), dp(10), false));
        t.setBackground(sl);
        return t;
    }

    /** Parallelogram (or, with leftCut, a rectangle whose left edge is cut on the diagonal). */
    private static final class SlantDrawable extends Drawable {
        private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStripe = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPath = new Path();
        private final int mSkew;
        private final boolean mLeftCut;
        private final boolean mHasStroke;

        SlantDrawable(int fill, int stroke, int strokeW, int skew, boolean leftCut) {
            mFill.setColor(fill);
            mFill.setStyle(Paint.Style.FILL);
            mHasStroke = strokeW > 0 && Color.alpha(stroke) > 0;
            mStroke.setColor(stroke);
            mStroke.setStyle(Paint.Style.STROKE);
            mStroke.setStrokeWidth(strokeW);
            mStripe.setColor(RED);
            mStripe.setStyle(Paint.Style.FILL);
            mSkew = skew;
            mLeftCut = leftCut;
        }

        @Override public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height();
            float s = Math.min(mSkew, w / 3f);
            mPath.reset();
            if (mLeftCut) {
                // left edge runs from top-right-of-corner down to the bottom-left: a diagonal cut
                mPath.moveTo(s, 0); mPath.lineTo(w, 0); mPath.lineTo(w, h); mPath.lineTo(0, h);
                mPath.close();
                c.drawPath(mPath, mFill);
                // red stripe hugging the diagonal
                Path stripe = new Path();
                stripe.moveTo(s, 0); stripe.lineTo(s + s * 0.35f, 0);
                stripe.lineTo(s * 0.35f, h); stripe.lineTo(0, h);
                stripe.close();
                c.drawPath(stripe, mStripe);
                return;
            }
            // parallelogram leaning right
            mPath.moveTo(s, 0); mPath.lineTo(w, 0); mPath.lineTo(w - s, h); mPath.lineTo(0, h);
            mPath.close();
            c.drawPath(mPath, mFill);
            if (mHasStroke) c.drawPath(mPath, mStroke);
        }

        @Override public void setAlpha(int a) { mFill.setAlpha(a); mStroke.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { mFill.setColorFilter(f); mStroke.setColorFilter(f); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** A thin line rising from bottom-left to top-right, used between rows. */
    private static final class DiagonalLineDrawable extends Drawable {
        private final Paint mP = new Paint(Paint.ANTI_ALIAS_FLAG);
        DiagonalLineDrawable(int color) {
            mP.setColor(color);
            mP.setStyle(Paint.Style.STROKE);
            mP.setStrokeWidth(2);
        }
        @Override public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height();
            c.drawLine(0, h - 1, w, 1, mP);
        }
        @Override public void setAlpha(int a) { mP.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { mP.setColorFilter(f); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** The badge: red slanted plate, white outline, black five-point star, tilted a few degrees. */
    private final class BadgeView extends View {
        private final Paint mFillP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mLineP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStarP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStarLineP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPlate = new Path();
        private final Path mStar = new Path();

        BadgeView(android.content.Context ctx) {
            super(ctx);
            mFillP.setColor(RED);
            mFillP.setStyle(Paint.Style.FILL);
            mLineP.setColor(Color.WHITE);
            mLineP.setStyle(Paint.Style.STROKE);
            mLineP.setStrokeWidth(dp(2));
            mStarP.setColor(BLACK);
            mStarP.setStyle(Paint.Style.FILL);
            mStarLineP.setColor(Color.WHITE);
            mStarLineP.setStyle(Paint.Style.STROKE);
            mStarLineP.setStrokeWidth(dp(1));
            setClickable(true);
        }

        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float m = dp(4), s = dp(12);
            c.save();
            c.rotate(-6f, w / 2f, h / 2f);
            mPlate.reset();
            mPlate.moveTo(m + s, m); mPlate.lineTo(w - m, m);
            mPlate.lineTo(w - m - s, h - m); mPlate.lineTo(m, h - m);
            mPlate.close();
            c.drawPath(mPlate, mFillP);
            c.drawPath(mPlate, mLineP);

            // five-point star, centre of the plate
            float cx = w / 2f, cy = h / 2f, ro = dp(13), ri = ro * 0.42f;
            mStar.reset();
            for (int i = 0; i < 10; i++) {
                double a = -Math.PI / 2 + i * Math.PI / 5;
                float r = (i % 2 == 0) ? ro : ri;
                float x = cx + (float) (Math.cos(a) * r);
                float y = cy + (float) (Math.sin(a) * r) + dp(1);
                if (i == 0) mStar.moveTo(x, y); else mStar.lineTo(x, y);
            }
            mStar.close();
            c.drawPath(mStar, mStarP);
            c.drawPath(mStar, mStarLineP);
            c.restore();
        }
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
