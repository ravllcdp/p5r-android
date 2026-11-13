package io.github.p5rrecomp;

import android.app.Service;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.os.IBinder;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
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
 * Draws two TYPE_APPLICATION_OVERLAY windows over the running game:
 *
 *  1. edgeView  — a 56 dp strip on the RIGHT edge of the screen. Always visible, always touchable.
 *                 Swipe LEFT on it → shows the settings panel.
 *                 Has a subtle "◄" indicator so the player can find it.
 *
 *  2. panelView — a 360 dp settings panel that slides in from the right.
 *                 Shows the most-used options (FPS, resolution, graphics, shadows, touch controls).
 *                 "Guardar"  → writes config.txt and hides the panel.
 *                 "✕" or swipe right on the edge → hides without saving.
 *
 * Most settings need a game restart to take effect; "Guardar" shows a toast about this.
 * Started by Launcher just before NativeActivity; lives for the life of the game process.
 */
public class OverlayService extends Service {

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
    private final List<Button> mOptBtns = new ArrayList<>();

    private WindowManager mWm;
    private View  mEdgeView;
    private View  mPanelRoot;
    private boolean mPanelOpen;

    // Touch tracking for swipe detection on the edge strip
    private float mTouchStartX, mTouchStartY;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (mWm != null) return START_STICKY; // already running
        mWm = (WindowManager) getSystemService(WINDOW_SERVICE);
        buildOptions();
        addEdgeView();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        if (mWm != null) {
            if (mEdgeView  != null && mEdgeView.getWindowToken()  != null) mWm.removeView(mEdgeView);
            if (mPanelRoot != null && mPanelRoot.getWindowToken() != null) mWm.removeView(mPanelRoot);
        }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent i) { return null; }

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
    // Edge strip
    // -----------------------------------------------------------------------

    private WindowManager.LayoutParams edgeParams() {
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                  | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                dp(56), WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.END | Gravity.CENTER_VERTICAL;
        return p;
    }

    private void addEdgeView() {
        FrameLayout edge = new FrameLayout(this) {
            @Override public boolean onTouchEvent(MotionEvent ev) {
                return handleEdgeTouch(ev);
            }
        };
        edge.setBackgroundColor(Color.argb(60, 230, 0, 18)); // subtle red hint

        TextView arrow = new TextView(this);
        arrow.setText("◄");
        arrow.setTextColor(Color.WHITE);
        arrow.setTextSize(18);
        arrow.setTypeface(Typeface.DEFAULT_BOLD);
        arrow.setGravity(Gravity.CENTER);
        edge.addView(arrow, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        mEdgeView = edge;
        mWm.addView(mEdgeView, edgeParams());
    }

    private boolean handleEdgeTouch(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mTouchStartX = ev.getRawX();
                mTouchStartY = ev.getRawY();
                return true;
            case MotionEvent.ACTION_UP:
                float dx = ev.getRawX() - mTouchStartX;
                float dy = ev.getRawY() - mTouchStartY;
                // Swipe left (into screen) with reasonable horizontal movement
                if (dx < -dp(40) && Math.abs(dy) < dp(80)) {
                    openPanel();
                }
                return true;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Settings panel
    // -----------------------------------------------------------------------

    private WindowManager.LayoutParams panelParams() {
        int flags = WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                dp(360), WindowManager.LayoutParams.MATCH_PARENT,
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
        root.setBackgroundColor(Color.argb(240, 16, 10, 12));

        // Header
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setBackgroundColor(Color.rgb(230, 0, 18));
        header.setPadding(dp(16), dp(10), dp(10), dp(10));

        TextView title = new TextView(this);
        title.setText("AJUSTES");
        title.setTextColor(Color.WHITE);
        title.setTextSize(18);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(title, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        Button closeBtn = new Button(this);
        closeBtn.setText("✕");
        closeBtn.setTextColor(Color.WHITE);
        closeBtn.setBackgroundColor(Color.TRANSPARENT);
        closeBtn.setTextSize(18);
        closeBtn.setOnClickListener(v -> closePanel());
        header.addView(closeBtn);
        root.addView(header);

        // Note
        TextView note = new TextView(this);
        note.setText("Los cambios requieren reiniciar el juego.");
        note.setTextColor(Color.rgb(180, 180, 180));
        note.setTextSize(12);
        note.setPadding(dp(16), dp(8), dp(16), dp(4));
        root.addView(note);

        // Options
        ScrollView scroll = new ScrollView(this);
        LinearLayout optList = new LinearLayout(this);
        optList.setOrientation(LinearLayout.VERTICAL);
        optList.setPadding(dp(16), dp(4), dp(16), dp(4));

        mOptBtns.clear();
        for (final Opt o : mOpts) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(6), 0, dp(6));

            TextView lbl = new TextView(this);
            lbl.setText(o.label);
            lbl.setTextColor(Color.WHITE);
            lbl.setTextSize(15);
            row.addView(lbl, new LinearLayout.LayoutParams(0,
                    LinearLayout.LayoutParams.WRAP_CONTENT, 1));

            Button btn = new Button(this);
            btn.setAllCaps(false);
            btn.setText(o.valLabels[o.pick]);
            btn.setTextColor(Color.WHITE);
            btn.setBackgroundColor(Color.argb(200, 50, 50, 50));
            btn.setTextSize(14);
            btn.setPadding(dp(10), dp(4), dp(10), dp(4));
            btn.setOnClickListener(v -> {
                o.pick = (o.pick + 1) % o.vals.length;
                btn.setText(o.valLabels[o.pick]);
            });
            mOptBtns.add(btn);
            row.addView(btn);
            optList.addView(row);
        }

        scroll.addView(optList);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        // Save button
        Button saveBtn = new Button(this);
        saveBtn.setText("Guardar");
        saveBtn.setAllCaps(false);
        saveBtn.setTextColor(Color.WHITE);
        saveBtn.setBackgroundColor(Color.rgb(230, 0, 18));
        saveBtn.setTextSize(16);
        LinearLayout.LayoutParams saveLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        saveLp.setMargins(dp(16), dp(8), dp(16), dp(16));
        saveBtn.setOnClickListener(v -> {
            saveConfig();
            android.widget.Toast.makeText(this,
                    "Guardado. Reinicia el juego para aplicar.",
                    android.widget.Toast.LENGTH_LONG).show();
            closePanel();
        });
        root.addView(saveBtn, saveLp);

        mPanelRoot = root;
        mWm.addView(mPanelRoot, panelParams());
    }

    private void closePanel() {
        if (!mPanelOpen) return;
        mPanelOpen = false;
        if (mPanelRoot != null && mPanelRoot.getWindowToken() != null) {
            mWm.removeView(mPanelRoot);
        }
        mPanelRoot = null;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
