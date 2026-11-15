package io.github.p5rrecomp;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import java.io.File;

/**
 * Opens the home screen again once the game's process has ended. Its own process (manifest), so killing the game's
 * does not take it along. Extras: "pid" (the process to wait for) and "mode" ("fresh" or "resume": what the home
 * screen does next, without asking).
 */
public class RelaunchService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        final int pid = intent.getIntExtra("pid", -1);
        final String mode = intent.getStringExtra("mode");
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    for (int i = 0; i < 100 && pid > 0 && new File("/proc/" + pid).exists(); i++) Thread.sleep(100);
                    Thread.sleep(400);  // let the system finish with the old process
                    startActivity(new Intent(RelaunchService.this, Launcher.class)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                            .putExtra("p5r_start", mode));
                } catch (Exception ignored) {
                }
                stopSelf();
            }
        }).start();
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent i) { return null; }
}
