package net.wexchat;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.webkit.WebView;
import android.webkit.WebSettings;
import android.webkit.WebViewClient;
import android.webkit.WebChromeClient;
import android.webkit.PermissionRequest;
import android.graphics.Color;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private WebView webView;

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

        // Start C99 Wex P2P Engine inside an unkillable Android Foreground Service
        Intent serviceIntent = new Intent(this, WexService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent);
        } else {
            startService(serviceIntent);
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

        // Check runtime permissions for mic and push notifications
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            List<String> perms = new ArrayList<String>();
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                perms.add(android.Manifest.permission.RECORD_AUDIO);
            }
            if (Build.VERSION.SDK_INT >= 33) {
                if (checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED) {
                    perms.add("android.permission.POST_NOTIFICATIONS");
                }
            }
            if (!perms.isEmpty()) {
                requestPermissions(perms.toArray(new String[0]), 101);
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
}
