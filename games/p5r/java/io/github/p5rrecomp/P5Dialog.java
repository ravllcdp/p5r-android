package io.github.p5rrecomp;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/** A question in the game's menu look (full screen, red slab title, white plate, two slanted buttons). */
final class P5Dialog {
    private static final int RED = Color.rgb(230, 0, 18);
    private static final int BLACK = Color.rgb(10, 10, 10);

    private P5Dialog() {}

    static void show(final Activity a, String title, String message, String yes, String no,
                     final Runnable onYes, final Runnable onNo) {
        final float density = a.getResources().getDisplayMetrics().density;
        final Dialog d = new Dialog(a, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        d.setCancelable(false);
        d.getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);

        FrameLayout back = new FrameLayout(a);
        back.setBackground(new OverlayService.MenuBackDrawable(Math.round(40 * density), Math.round(14 * density)));

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setClipChildren(false);
        FrameLayout.LayoutParams colLp = new FrameLayout.LayoutParams(Math.round(560 * density),
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        back.addView(col, colLp);

        TextView head = new TextView(a);
        head.setText(title);
        head.setAllCaps(true);
        head.setTextColor(Color.WHITE);
        head.setTextSize(30);
        head.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        head.setShadowLayer(0.01f, 3 * density, 3 * density, BLACK);
        head.setPadding(Math.round(34 * density), Math.round(14 * density), Math.round(40 * density), Math.round(20 * density));
        head.setBackground(new OverlayService.ShadowSlantDrawable(RED, Color.TRANSPARENT, 0, Color.WHITE,
                Math.round(5 * density), Math.round(28 * density)));
        head.setRotation(-2f);
        col.addView(head, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView msg = new TextView(a);
        msg.setText(message);
        msg.setTextColor(BLACK);
        msg.setTextSize(16);
        msg.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        msg.setPadding(Math.round(34 * density), Math.round(14 * density), Math.round(40 * density), Math.round(20 * density));
        msg.setBackground(new OverlayService.ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED,
                Math.round(4 * density), Math.round(16 * density)));
        LinearLayout.LayoutParams msgLp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        msgLp.setMargins(Math.round(30 * density), Math.round(22 * density), 0, 0);
        col.addView(msg, msgLp);

        LinearLayout buttons = new LinearLayout(a);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        TextView bYes = button(a, density, yes, true);
        TextView bNo = button(a, density, no, false);
        bYes.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { d.dismiss(); if (onYes != null) onYes.run(); }
        });
        bNo.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { d.dismiss(); if (onNo != null) onNo.run(); }
        });
        buttons.addView(bYes, new LinearLayout.LayoutParams(0, Math.round(64 * density), 1));
        buttons.addView(bNo, new LinearLayout.LayoutParams(0, Math.round(64 * density), 1));
        LinearLayout.LayoutParams bLp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        bLp.setMargins(Math.round(14 * density), Math.round(28 * density), 0, 0);
        col.addView(buttons, bLp);

        d.setContentView(back);
        d.show();

        // Entrance: title from the left, plate from the right, buttons rise
        head.setTranslationX(-400 * density); head.animate().translationX(0f).setDuration(220).start();
        msg.setAlpha(0f); msg.setTranslationX(300 * density);
        msg.animate().alpha(1f).translationX(0f).setStartDelay(120).setDuration(220).start();
        buttons.setAlpha(0f); buttons.setTranslationY(40 * density);
        buttons.animate().alpha(1f).translationY(0f).setStartDelay(240).setDuration(220).start();
    }

    private static TextView button(Activity a, float density, String text, boolean primary) {
        TextView t = new TextView(a);
        t.setText(text);
        t.setAllCaps(true);
        t.setGravity(Gravity.CENTER);
        t.setTextSize(18);
        t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        t.setClickable(true);
        int ink = primary ? RED : Color.WHITE, inkDown = primary ? Color.WHITE : RED;
        t.setTextColor(new android.content.res.ColorStateList(
                new int[][]{{android.R.attr.state_pressed}, {}}, new int[]{inkDown, ink}));
        int off = Math.round(4 * density), skew = Math.round(16 * density), sw = Math.round(2 * density);
        StateListDrawable bg = new StateListDrawable();
        if (primary) {
            bg.addState(new int[]{android.R.attr.state_pressed},
                    new OverlayService.ShadowSlantDrawable(RED, Color.WHITE, sw, Color.WHITE, off, skew));
            bg.addState(new int[]{},
                    new OverlayService.ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, off, skew));
        } else {
            bg.addState(new int[]{android.R.attr.state_pressed},
                    new OverlayService.ShadowSlantDrawable(Color.WHITE, Color.TRANSPARENT, 0, RED, off, skew));
            bg.addState(new int[]{},
                    new OverlayService.ShadowSlantDrawable(BLACK, Color.WHITE, sw, RED, off, skew));
        }
        t.setBackground(bg);
        return t;
    }
}
