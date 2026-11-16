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
    private View mSaveView;    // the second badge: one touch saves the game
    private View mBannerView;  // "Guardando" / "Partida guardada"
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
        mHandler.postDelayed(mPoll, 1000);
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mHideIcon);
        mHandler.removeCallbacks(mPoll);
        closeNotice();
        if (mWm != null) {
            removeQuiet(mListenView);
            removeQuiet(mIconView);
            removeQuiet(mSaveView);
            removeQuiet(mBannerView);
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
        mOpts.add(new Opt("language", "Idioma",
                new String[]{"auto","en-US","es","fr","de","it","ja","ko","zh-Hans","zh-Hant"},
                new String[]{"Auto","English","Español","Français","Deutsch","Italiano","日本語","한국어","简体中文","繁體中文"}, 0));
        mOpts.add(new Opt("RESOLUTION", "Resolución",
                new String[]{"360p","480p","720p","1080p"}, new String[]{"360p","480p","720p","1080p"}, 2));
        mOpts.add(new Opt("NX_STRETCH", "Pantalla completa",
                new String[]{"0","1"}, new String[]{"Desactivada","Activada"}, 0));
        mOpts.add(new Opt("NX_HUD_1080", "Interfaz",
                new String[]{"0","1"}, new String[]{"720p","1080p"}, 0));
        mOpts.add(new Opt("GRAPHICS", "Gráficos",
                new String[]{"low","medium","high"}, new String[]{"Bajos","Medios","Altos"}, 0));
        mOpts.add(new Opt("NX_FPS_CAP", "FPS",
                new String[]{"30","60"}, new String[]{"30 fps","60 fps"}, 0));
        mOpts.add(new Opt("NX_SHADOWS", "Sombras",
                new String[]{"0","1"}, new String[]{"Desactivadas","Activadas"}, 0));
        mOpts.add(new Opt("NX_SHOW_FPS", "Contador FPS",
                new String[]{"0","1"}, new String[]{"Desactivado","Activado"}, 0));
        mOpts.add(new Opt("NX_TOUCH_SHOW", "Controles táctiles",
                new String[]{"1","0"}, new String[]{"Activados","Desactivados"}, 0));
        mOpts.add(new Opt("NX_TOUCH_SIZE", "Tamaño de botones",
                new String[]{"80","100","125"}, new String[]{"Pequeño","Normal","Grande"}, 1));
        mOpts.add(new Opt("NX_TOUCH_OPACITY", "Opacidad de botones",
                new String[]{"50","85","100"}, new String[]{"Baja","Normal","Alta"}, 1));
        mOpts.add(new Opt("NX_NO_CROWD", "Multitudes",
                new String[]{"0","1"}, new String[]{"Activadas","Desactivadas"}, 0));
        mOpts.add(new Opt("NX_DISABLE_BLOOM", "Post-procesado",
                new String[]{"0","1"}, new String[]{"Activado","Desactivado"}, 0));
        mOpts.add(new Opt("NX_GAMEPAD_LAYOUT", "Botones del mando",
                new String[]{"0","1"}, new String[]{"Nintendo","Xbox / PC"}, 0));
        mOpts.add(new Opt("NX_STICK_SENSITIVITY", "Sensib. palanca",
                new String[]{"50","75","100","125","150","200"},
                new String[]{"50%","75%","100%","125%","150%","200%"}, 2));
        mOpts.add(new Opt("NX_CAMERA_SENSITIVITY", "Sensib. cámara",
                new String[]{"50","75","100","125","150","200","250"},
                new String[]{"50%","75%","100%","125%","150%","200%","250%"}, 2));
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
        p.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;  // top centre, the star to the right of the middle
        p.x = dp(33);
        p.y = dp(10);
        mIconView = badge;
        mWm.addView(mIconView, p);
        badge.animate().alpha(1f).setDuration(150).start();

        // Beside the star, to its left: a touch saves the game, no menu.
        View save = new SaveBadgeView(this);
        save.setAlpha(0f);
        save.setContentDescription("Guardar partida");
        save.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                mHandler.removeCallbacks(mHideIcon);
                removeIconNow();
                requestSave(null);
            }
        });
        WindowManager.LayoutParams sp = new WindowManager.LayoutParams(
                dp(64), dp(52), WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                flags, PixelFormat.TRANSLUCENT);
        sp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;  // and the save badge to the left of it
        sp.x = -dp(33);
        sp.y = dp(10);
        mSaveView = save;
        mWm.addView(mSaveView, sp);
        save.animate().alpha(1f).setDuration(150).start();
    }

    private void hideIcon() {
        if (!mIconShown || mIconView == null) return;
        final View badge = mIconView;
        if (mSaveView != null) mSaveView.animate().alpha(0f).setDuration(200).start();
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
        if (mSaveView != null) {
            mSaveView.animate().cancel();
            removeQuiet(mSaveView);
            mSaveView = null;
        }
    }

    // -----------------------------------------------------------------------
    // Saving the game (the runtime saves a state when it finds snap.dump: runtime snapshot.cpp)
    // -----------------------------------------------------------------------

    private File stateFile() {
        File f = new File(getFilesDir(), "snap/state.bin");
        return f.isFile() ? f : new File(getFilesDir(), "snap.old/state.bin");
    }

    private void requestSave(final Runnable onSaved) {
        final long asked = System.currentTimeMillis();
        try {
            File trigger = new File(android.os.Environment.getExternalStorageDirectory(), "Persona 5 Royal/snap.dump");
            //noinspection ResultOfMethodCallIgnored
            trigger.createNewFile();
        } catch (Exception e) {
            showBanner("No se pudo guardar la partida. La app no puede escribir en tus archivos. "
                    + "Revisa su permiso de archivos en los ajustes de Android.", 2);
            return;
        }
        // The small notice at the bottom shows it (the same one as the automatic saves): it stays while the runtime
        // saves and its going away means the state is saved; a failure shows the problem version of it.
        if (mSaveNotice == null) {
            mSawSaveFile = false;
            mSaveNotice = queueNotice(N_SAVING, asked);
        }
        if (onSaved == null) return;
        final Runnable[] poll = new Runnable[1];
        poll[0] = new Runnable() {
            @Override public void run() {
                File f = stateFile();
                if (f.isFile() && f.lastModified() >= asked - 1000) {
                    onSaved.run();
                } else if (System.currentTimeMillis() - asked <= 25000) {
                    mHandler.postDelayed(poll[0], 300);
                }
            }
        };
        mHandler.postDelayed(poll[0], 600);
    }

    // -----------------------------------------------------------------------
    // Small notices, bottom centre: "saving" (automatic or manual), "memory almost full", "could not save".
    // One at a time. The first to arrive goes first, the other waits for it to close; two that arrive together
    // (within 1.5 s: an emergency save makes both) show the memory warning first. The runtime leaves them as files in
    // the data root: notice.saving (while a save runs), notice.fail, notice.ram (docs/QUICK-RESUME.md).
    // -----------------------------------------------------------------------

    private static final int N_SAVING = 0, N_RAM = 1, N_FAIL = 2;
    private static final long TOGETHER_MS = 1500, SAVING_MIN_MS = 2000, RAM_MS = 5000, FAIL_MS = 4000;

    private static final class Notice {
        int kind;
        final long ts;
        boolean over;     // saving: the save is finished (the notice goes after its minimum time)
        long shownAt;
        Notice(int kind, long ts) { this.kind = kind; this.ts = ts; }
    }

    private final ArrayList<Notice> mQueue = new ArrayList<>();
    private Notice mCur;
    private Notice mSaveNotice;      // the notice of the save in progress (waiting or showing)
    private boolean mSawSaveFile;    // the runtime's notice.saving has been seen for it
    private View mNoticeView;
    private android.animation.ObjectAnimator mPulse;

    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            try { pollNotices(); } catch (Exception ignored) { }
            mHandler.postDelayed(this, 300);
        }
    };

    private File dataFile(String name) {
        return new File(android.os.Environment.getExternalStorageDirectory(), "Persona 5 Royal/" + name);
    }

    private long readStamp(File f) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
            return Long.parseLong(r.readLine().trim());
        } catch (Exception e) {
            return -1;
        }
    }

    private Notice queueNotice(int kind, long ts) {
        Notice n = new Notice(kind, ts);
        int at = mQueue.size();
        while (at > 0 && mQueue.get(at - 1).ts > ts) at--;
        mQueue.add(at, n);
        return n;
    }

    private void endSave(boolean failed) {
        Notice n = mSaveNotice;
        mSaveNotice = null;
        mSawSaveFile = false;
        if (n == null) {
            if (failed) queueNotice(N_FAIL, System.currentTimeMillis());
            return;
        }
        n.over = true;
        if (failed) {
            n.kind = N_FAIL;  // the same slot: shown now if it is, or when its turn comes
            if (n == mCur) {
                n.shownAt = System.currentTimeMillis();
                renderNotice();
            }
        }
    }

    private void pollNotices() {
        final long now = System.currentTimeMillis();
        File ram = dataFile("notice.ram");
        if (ram.exists()) {
            long ts = readStamp(ram);
            //noinspection ResultOfMethodCallIgnored
            ram.delete();
            if (ts < 0 || now - ts < 15000) queueNotice(N_RAM, ts < 0 ? now : ts);
        }
        boolean saving = false;
        long savingTs = now;
        File sv = dataFile("notice.saving");
        if (sv.exists()) {
            long ts = readStamp(sv);
            if (ts < 0 || now - ts < 60000) {  // older: left by a game that ended in the middle of a save
                saving = true;
                if (ts > 0) savingTs = ts;
            }
        }
        File fail = dataFile("notice.fail");
        boolean failed = fail.exists();
        //noinspection ResultOfMethodCallIgnored
        if (failed) fail.delete();

        if (saving) {
            mSawSaveFile = true;
            if (mSaveNotice == null) mSaveNotice = queueNotice(N_SAVING, savingTs);
        } else if (mSawSaveFile) {
            endSave(failed);
        } else if (mSaveNotice != null && now - mSaveNotice.ts > 25000) {
            endSave(true);  // asked, and the runtime never started (the game was busy)
        } else if (failed) {
            endSave(true);
        }

        if (mCur != null) {
            final long age = now - mCur.shownAt;
            boolean done;
            if (mCur.kind == N_SAVING) {
                done = (mCur.over && age >= SAVING_MIN_MS) || age > 40000;
                if (done && mCur == mSaveNotice) { mSaveNotice = null; mSawSaveFile = false; }
            } else {
                done = age >= (mCur.kind == N_RAM ? RAM_MS : FAIL_MS);
            }
            if (done) closeNotice();
        }
        if (mCur == null && !mQueue.isEmpty()) {
            Notice pick = mQueue.get(0);
            if (pick.kind == N_SAVING) {
                for (Notice n : mQueue) {
                    if (n.kind == N_RAM && n.ts <= pick.ts + TOGETHER_MS) { pick = n; break; }
                }
            }
            mQueue.remove(pick);
            mCur = pick;
            pick.shownAt = now;
            renderNotice();
        }
    }

    private void closeNotice() {
        if (mPulse != null) { mPulse.cancel(); mPulse = null; }
        if (mNoticeView != null) {
            mNoticeView.animate().cancel();
            if (mWm != null) removeQuiet(mNoticeView);
            mNoticeView = null;
        }
        mCur = null;
    }

    private void renderNotice() {
        final Notice n = mCur;
        if (n == null) return;
        if (mPulse != null) { mPulse.cancel(); mPulse = null; }
        if (mNoticeView != null) {
            mNoticeView.animate().cancel();
            removeQuiet(mNoticeView);
            mNoticeView = null;
        }
        final View v;
        int w = WindowManager.LayoutParams.WRAP_CONTENT, h = WindowManager.LayoutParams.WRAP_CONTENT;
        if (n.kind == N_SAVING) {
            // The save badge, small, breathing while it saves.
            v = new SaveBadgeView(this);
            v.setScaleX(0.6f);
            v.setScaleY(0.6f);
            w = dp(64);
            h = dp(52);
            mPulse = android.animation.ObjectAnimator.ofFloat(v, "alpha", 1f, 0.45f);
            mPulse.setDuration(600);
            mPulse.setRepeatCount(android.animation.ValueAnimator.INFINITE);
            mPulse.setRepeatMode(android.animation.ValueAnimator.REVERSE);
            mPulse.start();
        } else {
            boolean ram = n.kind == N_RAM;
            TextView t = new TextView(this);
            t.setText(ram ? "Memoria casi llena" : "No se pudo guardar");
            t.setAllCaps(true);
            t.setTextColor(ram ? RED : Color.WHITE);
            t.setTextSize(11);
            t.setLetterSpacing(0.06f);
            t.setSingleLine(true);
            t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
            t.setPadding(dp(16), dp(5), dp(16), dp(6));
            t.setBackground(new SlantDrawable(ram ? Color.WHITE : RED, ram ? RED : Color.WHITE, dp(1), dp(8), false));
            t.setRotation(-2f);
            t.setAlpha(0f);
            t.animate().alpha(1f).setDuration(150).start();
            v = t;
        }
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                  | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                w, h, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        p.y = dp(n.kind == N_SAVING ? 2 : 12);
        mNoticeView = v;
        mWm.addView(v, p);
    }

    private final Runnable mHideBanner = new Runnable() {
        @Override public void run() { removeBanner(); }
    };

    private void removeBanner() {
        mHandler.removeCallbacks(mHideBanner);
        if (mBannerView != null) {
            mBannerView.animate().cancel();
            removeQuiet(mBannerView);
            mBannerView = null;
        }
    }

    /** kind: 0 working (black), 1 done (red), 2 problem (white, red text). Drawn in capitals, written in sentence case. */
    private void showBanner(String text, int kind) {
        removeBanner();
        int fill = kind == 1 ? RED : kind == 2 ? Color.WHITE : BLACK;
        int ink = kind == 2 ? RED : Color.WHITE;
        TextView t = new TextView(this);
        t.setText(text);
        if (kind != 2) t.setAllCaps(true);  // the long problem text stays in sentence case, easier to read
        t.setTextColor(ink);
        t.setTextSize(kind == 2 ? 15 : 20);
        t.setMaxWidth(dp(360));
        t.setLetterSpacing(0.06f);
        t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        t.setPadding(dp(30), dp(10), dp(30), dp(12));
        t.setBackground(new SlantDrawable(fill, kind == 0 ? RED : Color.WHITE, dp(2), dp(14), false));
        t.setRotation(-2f);
        t.setAlpha(0f);
        t.setTranslationX(dp(40));
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                  | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, flags, PixelFormat.TRANSLUCENT);
        p.gravity = Gravity.TOP | Gravity.END;
        p.x = dp(14);
        p.y = dp(14);
        mBannerView = t;
        mWm.addView(t, p);
        t.animate().alpha(1f).translationX(0f).setDuration(160).start();
        if (kind != 0) mHandler.postDelayed(mHideBanner, kind == 1 ? 2200 : 6000);
    }

    private String lastSaveLine() {
        File f = stateFile();
        if (!f.isFile()) return "Sin guardado";
        CharSequence ago = android.text.format.DateUtils.getRelativeTimeSpanString(
                f.lastModified(), System.currentTimeMillis(), android.text.format.DateUtils.MINUTE_IN_MILLIS);
        return "Guardado: " + ago;
    }

    // -----------------------------------------------------------------------
    // Restart and load: the game's process ends, a helper process opens the home screen again
    // -----------------------------------------------------------------------

    private void applyAndRestart() {
        // Save first, so nothing is lost; if it cannot be saved the game is left running (the banner says why).
        requestSave(new Runnable() {
            @Override public void run() { restartGame("fresh"); }
        });
    }

    private void loadSavedGame() {
        try {
            new File(android.os.Environment.getExternalStorageDirectory(), "Persona 5 Royal/snap.restore").createNewFile();
        } catch (Exception ignored) { }
        restartGame("resume");
    }

    private void restartGame(String mode) {
        showBanner(mode.equals("resume") ? "Cargando partida…" : "Reiniciando el juego…", 0);
        startService(new Intent(this, RelaunchService.class)
                .putExtra("pid", android.os.Process.myPid())
                .putExtra("mode", mode));
        mHandler.postDelayed(new Runnable() {
            @Override public void run() { android.os.Process.killProcess(android.os.Process.myPid()); }
        }, 900);
    }

    /** A wide slanted button: white slab with red ink (primary) or red slab with white ink; inverts on press. */
    private TextView plate(String text, boolean white, int sp) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setAllCaps(true);
        t.setGravity(Gravity.CENTER);
        t.setTextSize(sp);
        t.setSingleLine(true);
        t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        int ink = white ? RED : Color.WHITE, inkDown = white ? Color.WHITE : RED;
        t.setTextColor(new android.content.res.ColorStateList(
                new int[][]{{android.R.attr.state_pressed}, {}}, new int[]{inkDown, ink}));
        t.setClickable(true);
        StateListDrawable bg = new StateListDrawable();
        if (white) {
            bg.addState(new int[]{android.R.attr.state_pressed},
                    new ShadowSlantDrawable(RED, Color.WHITE, dp(2), Color.WHITE, dp(4), dp(16)));
            bg.addState(new int[]{},
                    new ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, dp(4), dp(16)));
        } else {
            bg.addState(new int[]{android.R.attr.state_pressed},
                    new ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, dp(4), dp(16)));
            bg.addState(new int[]{},
                    new ShadowSlantDrawable(RED, Color.TRANSPARENT, 0, Color.WHITE, dp(4), dp(16)));
        }
        t.setBackground(bg);
        return t;
    }

    // -----------------------------------------------------------------------
    // Settings panel
    // -----------------------------------------------------------------------

    private WindowManager.LayoutParams panelParams() {
        int flags = WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                  | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                dp(420), WindowManager.LayoutParams.MATCH_PARENT,
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
        root.setBackground(new MenuBackDrawable(dp(40), dp(14)));
        root.setPadding(0, 0, 0, 0);

        // Header: red slab with a white offset, title tilted with a hard black shadow, close badge
        FrameLayout header = new FrameLayout(this);
        // The header starts exactly on the outer edge of the red bar: that edge runs from x = 40 dp at the top of the
        // panel to x = 0 at the bottom, so over the header's height it leans by 40 dp * height / panel height.
        final int cut = dp(40), headerH = dp(70);
        final int panelH = Math.max(headerH * 2, getResources().getDisplayMetrics().heightPixels);
        final int lean = Math.round(cut * (float) headerH / panelH);
        header.setBackground(new ShadowSlantDrawable(RED, Color.TRANSPARENT, 0, Color.WHITE, dp(4), dp(22)).leftSkew(lean));
        header.setPadding(dp(32), dp(10), dp(18), dp(14));
        header.setClipChildren(false);

        TextView title = new TextView(this);
        title.setText("Ajustes");
        title.setAllCaps(true);
        title.setTextColor(Color.WHITE);
        title.setTextSize(34);
        title.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        title.setLetterSpacing(0.05f);
        title.setShadowLayer(0.01f, dp(3), dp(3), BLACK);
        title.setRotation(-3f);
        header.addView(title, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_VERTICAL | Gravity.START));

        TextView close = slantButton("X", BLACK, Color.rgb(60, 60, 60), Color.WHITE, 16, dp(42), dp(34));
        close.setContentDescription("Cerrar ajustes");
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { closePanel(); }
        });
        header.addView(close, new FrameLayout.LayoutParams(dp(42), dp(34), Gravity.CENTER_VERTICAL | Gravity.END));
        LinearLayout.LayoutParams headerLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        headerLp.setMargins(cut - lean, 0, dp(4), 0);
        root.addView(header, headerLp);

        // One row: the last save (white plate) and the button to load it
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);

        TextView last = new TextView(this);
        last.setText(lastSaveLine());
        last.setAllCaps(true);
        last.setTextColor(BLACK);
        last.setTextSize(12);
        last.setSingleLine(true);
        last.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        last.setPadding(dp(20), dp(5), dp(20), dp(8));
        last.setBackground(new ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, dp(3), dp(10)));
        bar.addView(last, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // Load the last save (asks twice: it throws away what has not been saved)
        final boolean hasSave = stateFile().isFile();
        final TextView load = plate("Cargar partida", hasSave, 13);
        if (!hasSave) load.setAlpha(0.55f);
        final boolean[] armed = {false};
        load.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (!hasSave) {
                    showBanner("Aún no hay partida guardada. Toca el icono de guardar para crear una.", 2);
                    return;
                }
                if (!armed[0]) {
                    armed[0] = true;
                    load.setText("Descartar y cargar");
                    mHandler.postDelayed(new Runnable() {
                        @Override public void run() {
                            if (armed[0]) { armed[0] = false; load.setText("Cargar partida"); }
                        }
                    }, 5000);
                    return;
                }
                closePanel();
                loadSavedGame();
            }
        });
        LinearLayout.LayoutParams loadLp = new LinearLayout.LayoutParams(0, dp(38), 1);
        loadLp.setMargins(dp(10), 0, 0, 0);
        bar.addView(load, loadLp);

        LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        barLp.setMargins(dp(54), dp(14), dp(16), dp(2));
        root.addView(bar, barLp);

        // Options: one slanted strip each; the whole strip is the button
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        LinearLayout optList = new LinearLayout(this);
        optList.setOrientation(LinearLayout.VERTICAL);
        optList.setPadding(dp(52), dp(6), dp(12), dp(8));

        final android.content.res.ColorStateList labelInk = new android.content.res.ColorStateList(
                new int[][]{{android.R.attr.state_pressed}, {}}, new int[]{RED, Color.WHITE});
        final android.content.res.ColorStateList valueInk = new android.content.res.ColorStateList(
                new int[][]{{android.R.attr.state_pressed}, {}}, new int[]{Color.WHITE, Color.WHITE});
        final List<View> rows = new ArrayList<>();

        int idx = 0;
        for (final Opt o : mOpts) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setClickable(true);
            row.setPadding(dp(26), dp(4), dp(22), dp(8));
            StateListDrawable rowBg = new StateListDrawable();
            rowBg.addState(new int[]{android.R.attr.state_pressed},
                    new ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, dp(4), dp(16)));
            rowBg.addState(new int[]{},
                    new ShadowSlantDrawable(BLACK, Color.WHITE, dp(2), RED, dp(4), dp(16)));
            row.setBackground(rowBg);
            row.setRotation(-1.2f);
            row.setContentDescription(o.label);

            TextView lbl = new TextView(this);
            lbl.setText(o.label);
            lbl.setAllCaps(true);
            lbl.setTextColor(labelInk);
            lbl.setTextSize(15);
            lbl.setMaxLines(2);
            lbl.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
            lbl.setDuplicateParentStateEnabled(true);
            row.addView(lbl, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

            final TextView val = new TextView(this);
            val.setText(o.valLabels[o.pick]);
            val.setAllCaps(true);
            val.setTextColor(valueInk);
            val.setTextSize(13);
            val.setGravity(Gravity.CENTER);
            val.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
            val.setDuplicateParentStateEnabled(true);
            StateListDrawable valBg = new StateListDrawable();
            valBg.addState(new int[]{android.R.attr.state_pressed},
                    new ShadowSlantDrawable(BLACK, Color.WHITE, dp(1), Color.TRANSPARENT, 0, dp(8)));
            valBg.addState(new int[]{},
                    new ShadowSlantDrawable(RED, Color.TRANSPARENT, 0, Color.TRANSPARENT, 0, dp(8)));
            val.setBackground(valBg);
            row.addView(val, new LinearLayout.LayoutParams(dp(140), dp(34)));

            row.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    o.pick = (o.pick + 1) % o.vals.length;
                    val.setText(o.valLabels[o.pick]);
                }
            });
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(56));
            rowLp.setMargins((idx % 2) * dp(10), dp(4), 0, dp(6));
            optList.addView(row, rowLp);
            row.setAlpha(0f);
            row.setTranslationX(dp(70));
            rows.add(row);
            idx++;
        }

        scroll.addView(optList);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);

        TextView apply = plate("Aplicar ajustes", true, 13);
        apply.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                saveConfig();
                android.widget.Toast.makeText(OverlayService.this,
                        "Ajustes aplicados. Se usarán la próxima vez que abras el juego.",
                        android.widget.Toast.LENGTH_LONG).show();
                closePanel();
            }
        });
        actions.addView(apply, new LinearLayout.LayoutParams(0, dp(44), 1));

        TextView restart = plate("Aplicar y reiniciar", false, 13);
        restart.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                saveConfig();
                closePanel();
                applyAndRestart();
            }
        });
        LinearLayout.LayoutParams restartLp = new LinearLayout.LayoutParams(0, dp(44), 1);
        restartLp.setMargins(dp(8), 0, 0, 0);
        actions.addView(restart, restartLp);

        LinearLayout.LayoutParams actionsLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        actionsLp.setMargins(dp(50), dp(6), dp(16), dp(14));
        root.addView(actions, actionsLp);

        mPanelRoot = root;
        mWm.addView(mPanelRoot, panelParams());

        // Entrance: the panel slides in, then the strips one after another
        root.setTranslationX(dp(440));
        root.animate().translationX(0f).setDuration(200).start();
        for (int i = 0; i < rows.size(); i++) {
            rows.get(i).animate().alpha(1f).translationX(0f).setStartDelay(140 + i * 45L).setDuration(180).start();
        }
    }

    private void closePanel() {
        if (!mPanelOpen) return;
        mPanelOpen = false;
        final View panel = mPanelRoot;
        mPanelRoot = null;
        if (panel == null) return;
        panel.animate().cancel();
        panel.animate().translationX(dp(440)).setDuration(190).withEndAction(new Runnable() {
            @Override public void run() { removeQuiet(panel); }
        }).start();
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
        t.setAllCaps(true);
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

    /** The panel's back: black with a halftone fading down, a dark red star, and the left edge cut on a red and white diagonal. */
    static final class MenuBackDrawable extends Drawable {
        private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mDots = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStar = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mRed = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mWhite = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPath = new Path();
        private final int mSkew, mGrid;

        MenuBackDrawable(int skew, int grid) {
            mSkew = skew; mGrid = grid;
            mFill.setColor(BLACK);
            mDots.setColor(Color.rgb(38, 38, 38));
            mDots.setStrokeCap(Paint.Cap.ROUND);
            mStar.setColor(Color.rgb(46, 0, 5));
            mRed.setColor(RED);
            mWhite.setColor(Color.WHITE);
        }

        @Override public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height(), s = mSkew;
            mPath.reset();
            mPath.moveTo(s, 0); mPath.lineTo(w, 0); mPath.lineTo(w, h); mPath.lineTo(0, h);
            mPath.close();
            c.save();
            c.clipPath(mPath);
            c.drawPaint(mFill);

            // halftone: bigger dots at the top, none at the bottom
            for (int band = 0; band < 4; band++) {
                float y0 = h * band / 5f, y1 = h * (band + 1) / 5f;
                mDots.setStrokeWidth(mGrid * (0.62f - band * 0.14f));
                float[] pts = new float[(int) (w / mGrid + 2) * (int) ((y1 - y0) / mGrid + 2) * 2];
                int n = 0;
                int row = 0;
                for (float y = y0; y < y1; y += mGrid, row++) {
                    for (float x = (row % 2) * mGrid / 2f; x < w; x += mGrid) {
                        pts[n++] = x; pts[n++] = y;
                    }
                }
                c.drawPoints(pts, 0, n, mDots);
            }

            // star watermark, low right
            float cx = w * 0.62f, cy = h * 0.8f, ro = Math.min(w, h) * 0.62f, ri = ro * 0.42f;
            Path star = new Path();
            for (int i = 0; i < 10; i++) {
                double a = -Math.PI / 2 + i * Math.PI / 5 + 0.2;
                float r = (i % 2 == 0) ? ro : ri;
                float x = cx + (float) (Math.cos(a) * r), y = cy + (float) (Math.sin(a) * r);
                if (i == 0) star.moveTo(x, y); else star.lineTo(x, y);
            }
            star.close();
            c.drawPath(star, mStar);
            c.restore();

            // the cut: a wide red stripe and a thin white one along the diagonal
            Path red = new Path();
            red.moveTo(s, 0); red.lineTo(s + s * 0.42f, 0); red.lineTo(s * 0.42f, h); red.lineTo(0, h);
            red.close();
            c.drawPath(red, mRed);
            Path white = new Path();
            white.moveTo(s + s * 0.5f, 0); white.lineTo(s + s * 0.6f, 0); white.lineTo(s * 0.6f, h); white.lineTo(s * 0.5f, h);
            white.close();
            c.drawPath(white, mWhite);
        }

        @Override public void setAlpha(int a) { }
        @Override public void setColorFilter(ColorFilter f) { }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** A parallelogram with a hard offset copy behind it (the game's plates). The shape leaves room inside its bounds for the offset. */
    static final class ShadowSlantDrawable extends Drawable {
        private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mShadow = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPath = new Path();
        private final int mSkew, mOff;
        private final boolean mHasStroke, mHasShadow;
        private int mLeftSkew = -1;

        /** How far the left edge leans (default: as much as the right one). */
        ShadowSlantDrawable leftSkew(int px) { mLeftSkew = px; return this; }

        ShadowSlantDrawable(int fill, int stroke, int strokeW, int shadow, int off, int skew) {
            mFill.setColor(fill);
            mFill.setStyle(Paint.Style.FILL);
            mHasStroke = strokeW > 0 && Color.alpha(stroke) > 0;
            mStroke.setColor(stroke);
            mStroke.setStyle(Paint.Style.STROKE);
            mStroke.setStrokeWidth(strokeW);
            mStroke.setStrokeJoin(Paint.Join.MITER);
            mHasShadow = off > 0 && Color.alpha(shadow) > 0;
            mShadow.setColor(shadow);
            mShadow.setStyle(Paint.Style.FILL);
            mSkew = skew; mOff = off;
        }

        private void shape(float w, float h, float dx, float dy) {
            float s = Math.min(mSkew, w / 3f);
            mPath.reset();
            mPath.moveTo(dx + (mLeftSkew >= 0 ? mLeftSkew : s), dy); mPath.lineTo(dx + w, dy);
            mPath.lineTo(dx + w - s, dy + h); mPath.lineTo(dx, dy + h);
            mPath.close();
        }

        @Override public void draw(Canvas c) {
            float w = getBounds().width() - mOff, h = getBounds().height() - mOff;
            if (mHasShadow) {
                shape(w, h, mOff, mOff);
                c.drawPath(mPath, mShadow);
            }
            shape(w, h, 0, 0);
            c.drawPath(mPath, mFill);
            if (mHasStroke) c.drawPath(mPath, mStroke);
        }

        @Override public void setAlpha(int a) { mFill.setAlpha(a); mStroke.setAlpha(a); mShadow.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { mFill.setColorFilter(f); mStroke.setColorFilter(f); mShadow.setColorFilter(f); }
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
            mFillP.setColor(BLACK);
            mFillP.setStyle(Paint.Style.FILL);
            mLineP.setColor(RED);
            mLineP.setStyle(Paint.Style.STROKE);
            mLineP.setStrokeWidth(dp(3));
            mStarP.setColor(Color.WHITE);
            mStarP.setStyle(Paint.Style.FILL);
            mStarLineP.setColor(Color.TRANSPARENT);
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

    /** The save badge: black slanted plate, red outline, white arrow into a tray; tilted the other way from the star's. */
    private final class SaveBadgeView extends View {
        private final Paint mPlateP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mLineP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint mInkP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path mPlate = new Path();
        private final Path mArrow = new Path();

        SaveBadgeView(android.content.Context ctx) {
            super(ctx);
            mPlateP.setColor(BLACK);
            mPlateP.setStyle(Paint.Style.FILL);
            mLineP.setColor(RED);
            mLineP.setStyle(Paint.Style.STROKE);
            mLineP.setStrokeWidth(dp(3));
            mInkP.setColor(Color.WHITE);
            mInkP.setStyle(Paint.Style.FILL);
            setClickable(true);
        }

        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float m = dp(4), s = dp(12);
            c.save();
            c.rotate(5f, w / 2f, h / 2f);
            mPlate.reset();
            mPlate.moveTo(m + s, m); mPlate.lineTo(w - m, m);
            mPlate.lineTo(w - m - s, h - m); mPlate.lineTo(m, h - m);
            mPlate.close();
            c.drawPath(mPlate, mPlateP);
            c.drawPath(mPlate, mLineP);

            float cx = w / 2f, cy = h / 2f - dp(3);
            mArrow.reset();
            mArrow.moveTo(cx - dp(3), cy - dp(11)); mArrow.lineTo(cx + dp(3), cy - dp(11));
            mArrow.lineTo(cx + dp(3), cy - dp(1)); mArrow.lineTo(cx + dp(9), cy - dp(1));
            mArrow.lineTo(cx, cy + dp(9)); mArrow.lineTo(cx - dp(9), cy - dp(1));
            mArrow.lineTo(cx - dp(3), cy - dp(1));
            mArrow.close();
            c.drawPath(mArrow, mInkP);
            c.drawRect(cx - dp(11), cy + dp(12), cx + dp(11), cy + dp(15), mInkP);
            c.restore();
        }
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
