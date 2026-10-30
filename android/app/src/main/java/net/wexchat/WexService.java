package net.wexchat;

import android.app.Service;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import java.io.File;

public class WexService extends Service {
    static {
        System.loadLibrary("wex");
    }

    public static final String CHANNEL_ID = "wex_p2p_channel";
    public static final int NOTIFICATION_ID = 1001;

    public native int startNativeDaemon(String dataDir, int wsPort, int udpPort, String nickname, String bootstrap);
    public native void stopNativeDaemon();
    public native boolean isNativeDaemonRunning();

    private PowerManager.WakeLock wakeLock;
    private static boolean daemonRunning = false;

    @Override
    public void onCreate() {
        super.onCreate();

        // 1. Acquire partial wake lock to prevent CPU sleep during background mesh routing
        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "WexChat:P2PWakeLock");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
        }

        // 2. Setup Notification Channel for Android O+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "Wex-Chat P2P Daemon",
                NotificationManager.IMPORTANCE_LOW
            );
            channel.setDescription("Background P2P swarm routing and encrypted sync");
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm != null) {
                nm.createNotificationChannel(channel);
            }
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // 3. Build ongoing foreground notification
        Intent notificationIntent = new Intent(this, MainActivity.class);
        int pendingFlags = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, notificationIntent, pendingFlags);

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        Notification notification = builder
            .setContentTitle("Wex-Chat P2P Active")
            .setContentText("Sovereign encrypted mesh connected in background")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build();

        // 4. Start as Foreground Service
        if (Build.VERSION.SDK_INT >= 34) {
            try {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } catch (Throwable t) {
                startForeground(NOTIFICATION_ID, notification);
            }
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }

        // 5. Start native C99 Wex P2P engine in background worker thread
        if (!daemonRunning) {
            File filesDir = getFilesDir();
            File dataDir = new File(filesDir, ".wex_data");
            if (!dataDir.exists()) {
                dataDir.mkdirs();
            }
            String defaultNick = (Build.MODEL != null && !Build.MODEL.trim().isEmpty())
                ? Build.MODEL.trim()
                : "AndroidPeer";
            startNativeDaemon(dataDir.getAbsolutePath(), 9000, 9001, defaultNick, "");
            daemonRunning = true;
        }

        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        if (daemonRunning) {
            stopNativeDaemon();
            daemonRunning = false;
        }
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        stopForeground(true);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
