package net.wexchat;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.content.pm.PackageManager;
import android.webkit.WebView;
import android.webkit.WebSettings;
import android.webkit.WebViewClient;
import android.webkit.WebChromeClient;
import android.webkit.PermissionRequest;
import android.graphics.Color;
import java.io.File;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("wex");
    }

    public native int startNativeDaemon(String dataDir, int wsPort, int udpPort, String nickname, String bootstrap);
    public native void stopNativeDaemon();
    public native boolean isNativeDaemonRunning();

    private WebView webView;
    private static boolean daemonStarted = false;

    private static class WexChromeClient extends WebChromeClient {
        @Override
        public void onPermissionRequest(PermissionRequest request) {
            if (request != null && request.getResources() != null) {
                request.grant(request.getResources());
            }
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Start embedded C99 Wex P2P Daemon in background thread
        if (!daemonStarted) {
            File filesDir = getFilesDir();
            File dataDir = new File(filesDir, ".wex_data");
            if (!dataDir.exists()) {
                dataDir.mkdirs();
            }
            startNativeDaemon(dataDir.getAbsolutePath(), 9000, 9001, "AndroidPeer", "");
            daemonStarted = true;
        }

        webView = new WebView(this);
        webView.setBackgroundColor(Color.parseColor("#313338"));
        setContentView(webView);

        WebSettings s = webView.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        s.setAllowContentAccess(true);
        s.setDatabaseEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);

        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WexChromeClient());

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO}, 101);
            }
        }

        webView.loadUrl("file:///android_asset/index.html");
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            stopNativeDaemon();
            daemonStarted = false;
        }
    }
}
