package io.github.p5rrecomp;

import android.app.Presentation;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Display;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * Displays live party HP/SP data on the AYN Thor's second physical screen (Display #4,
 * FLAG_PRESENTATION). Polls the C++ game state at 10 Hz via JNI (nativeGetSnapshot).
 * Started by Launcher before NativeActivity; runs for the life of the game process.
 */
public class SecondScreenService extends Service {

    static {
        try { System.loadLibrary("p5r"); } catch (UnsatisfiedLinkError ignored) {}
    }

    native int[] nativeGetSnapshot();

    // P5R member names indexed by id (0=Joker, matches p5r_offsets.h kUnitMemberId).
    private static final String[] NAMES = {
        "Joker", "Ryuji", "Morgana", "Ann", "Yusuke",
        "Makoto", "Futaba", "Haru", "Akechi", "Kasumi", "Lavenza"
    };

    private Presentation      mPresentation;
    private final Handler     mHandler = new Handler(Looper.getMainLooper());
    private boolean           mRunning;

    private final TextView[]    mName  = new TextView[4];
    private final ProgressBar[] mHp    = new ProgressBar[4];
    private final ProgressBar[] mSp    = new ProgressBar[4];
    private final TextView[]    mHpTxt = new TextView[4];
    private final TextView[]    mSpTxt = new TextView[4];

    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            if (!mRunning) return;
            tick();
            mHandler.postDelayed(this, 100);  // 10 Hz
        }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (mRunning) return START_STICKY;
        mRunning = true;

        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display[] all = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (all != null && all.length > 0) {
            // AYN Thor exposes Display #4 "Screen-2" as the last PRESENTATION display.
            GamePresentation p = new GamePresentation(this, all[all.length - 1]);
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

    // Called on the main thread every 100 ms. Reads JNI snapshot and updates views.
    private void tick() {
        if (mPresentation == null || !mPresentation.isShowing()) return;
        int[] s;
        try { s = nativeGetSnapshot(); } catch (Throwable t) { return; }
        // Array layout: [0]=valid [1]=in_battle, then 4×8 slots starting at index 2.
        if (s == null || s.length < 34) return;
        final boolean valid = s[0] == 1;
        for (int i = 0; i < 4; i++) {
            int b = 2 + i * 8;
            boolean on   = valid && s[b] == 1;
            int id    = s[b + 1];
            int hp    = s[b + 2], hpMax = s[b + 3];
            int sp    = s[b + 4], spMax = s[b + 5];
            int lv    = s[b + 6];
            String nm = (id >= 0 && id < NAMES.length) ? NAMES[id] : "?";

            mName[i].setText(on ? "Lv." + lv + "  " + nm : "—");
            mName[i].setAlpha(on ? 1f : 0.25f);
            mHpTxt[i].setText(on ? hp + "/" + hpMax : "");
            mSpTxt[i].setText(on ? sp + "/" + spMax : "");
            mHp[i].setMax(on && hpMax > 0 ? hpMax : 1);
            mHp[i].setProgress(on ? hp : 0);
            mSp[i].setMax(on && spMax > 0 ? spMax : 1);
            mSp[i].setProgress(on ? sp : 0);
        }
    }

    // ---------------------------------------------------------------------------

    private class GamePresentation extends Presentation {
        GamePresentation(Context ctx, Display d) {
            super(ctx, d);
            buildUi();
        }

        private void buildUi() {
            LinearLayout root = new LinearLayout(getContext());
            root.setOrientation(LinearLayout.VERTICAL);
            root.setBackgroundColor(Color.rgb(10, 2, 3));
            root.setPadding(40, 48, 40, 48);

            TextView title = new TextView(getContext());
            title.setText("PERSONA 5 ROYAL");
            title.setTextColor(Color.rgb(230, 0, 18));
            title.setTextSize(20);
            title.setPadding(0, 0, 0, 32);
            root.addView(title);

            for (int i = 0; i < 4; i++) {
                LinearLayout slot = new LinearLayout(getContext());
                slot.setOrientation(LinearLayout.VERTICAL);
                slot.setPadding(0, 0, 0, 24);

                mName[i] = new TextView(getContext());
                mName[i].setTextColor(Color.WHITE);
                mName[i].setTextSize(16);
                mName[i].setText("—");
                slot.addView(mName[i]);

                slot.addView(barRow("HP", Color.rgb(80, 200, 80),  i, true));
                slot.addView(barRow("SP", Color.rgb(80, 130, 230), i, false));
                root.addView(slot);
            }
            setContentView(root);
        }

        private LinearLayout barRow(String label, int color, int idx, boolean hp) {
            LinearLayout row = new LinearLayout(getContext());
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, 4, 0, 4);

            TextView lbl = new TextView(getContext());
            lbl.setText(label);
            lbl.setTextColor(color);
            lbl.setTextSize(12);
            lbl.setMinWidth(36);
            row.addView(lbl);

            ProgressBar bar = new ProgressBar(getContext(), null, android.R.attr.progressBarStyleHorizontal);
            bar.setProgressTintList(ColorStateList.valueOf(color));
            bar.setProgressBackgroundTintList(ColorStateList.valueOf(Color.rgb(50, 50, 50)));
            row.addView(bar, new LinearLayout.LayoutParams(0, 20, 1f));

            TextView txt = new TextView(getContext());
            txt.setTextColor(Color.rgb(190, 190, 190));
            txt.setTextSize(12);
            txt.setPadding(10, 0, 0, 0);
            txt.setMinWidth(100);
            row.addView(txt);

            if (hp) { mHp[idx] = bar; mHpTxt[idx] = txt; }
            else    { mSp[idx] = bar; mSpTxt[idx] = txt; }

            return row;
        }
    }
}
