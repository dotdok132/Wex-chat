package net.wexchat;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.os.Build;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebSettings;
import android.webkit.WebViewClient;
import android.webkit.WebChromeClient;
import android.webkit.PermissionRequest;
import android.webkit.JsResult;
import android.webkit.JsPromptResult;
import android.widget.EditText;
import android.graphics.Color;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private WebView webView;

    public static class WexAndroidBridge {
        private final Context ctx;

        public WexAndroidBridge(Context ctx) {
            this.ctx = ctx;
        }

        @JavascriptInterface
        public void copyText(String text) {
            if (text == null) return;
            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                cm.setPrimaryClip(ClipData.newPlainText("Wex-Chat", text));
            }
        }

        @JavascriptInterface
        public String pasteText() {
            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null && cm.hasPrimaryClip()) {
                ClipData clip = cm.getPrimaryClip();
                if (clip != null && clip.getItemCount() > 0 && clip.getItemAt(0).getText() != null) {
                    return clip.getItemAt(0).getText().toString();
                }
            }
            return "";
        }

        @JavascriptInterface
        public void shareText(String text) {
            if (text == null) return;
            Intent sendIntent = new Intent(Intent.ACTION_SEND);
            sendIntent.putExtra(Intent.EXTRA_TEXT, text);
            sendIntent.setType("text/plain");
            Intent shareIntent = Intent.createChooser(sendIntent, "Поделиться инвайтом Wex-Chat");
            shareIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(shareIntent);
        }
    }

    private static class WexChromeClient extends WebChromeClient {
        @Override
        public void onPermissionRequest(PermissionRequest request) {
            if (request != null && request.getResources() != null) {
                request.grant(request.getResources());
            }
        }

        @Override
        public boolean onJsAlert(WebView view, String url, String message, final JsResult result) {
            new AlertDialog.Builder(view.getContext())
                .setTitle("Wex-Chat")
                .setMessage(message)
                .setPositiveButton("OK", (dialog, which) -> result.confirm())
                .setCancelable(false)
                .show();
            return true;
        }

        @Override
        public boolean onJsConfirm(WebView view, String url, String message, final JsResult result) {
            new AlertDialog.Builder(view.getContext())
                .setTitle("Wex-Chat")
                .setMessage(message)
                .setPositiveButton("Да", (dialog, which) -> result.confirm())
                .setNegativeButton("Отмена", (dialog, which) -> result.cancel())
                .setCancelable(false)
                .show();
            return true;
        }

        @Override
        public boolean onJsPrompt(WebView view, String url, String message, String defaultValue, final JsPromptResult result) {
            final EditText input = new EditText(view.getContext());
            if (defaultValue != null) {
                input.setText(defaultValue);
                input.selectAll();
            }
            new AlertDialog.Builder(view.getContext())
                .setTitle("Wex-Chat")
                .setMessage(message)
                .setView(input)
                .setPositiveButton("OK", (dialog, which) -> result.confirm(input.getText().toString()))
                .setNegativeButton("Отмена", (dialog, which) -> result.cancel())
                .setCancelable(false)
                .show();
            return true;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            Window window = getWindow();
            window.clearFlags(WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS);
            window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
            window.setStatusBarColor(Color.parseColor("#1e1f22"));
            window.setNavigationBarColor(Color.parseColor("#1e1f22"));
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }

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
        s.setAllowFileAccessFromFileURLs(true);
        s.setAllowUniversalAccessFromFileURLs(true);
        s.setDatabaseEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);

        webView.addJavascriptInterface(new WexAndroidBridge(this), "AndroidBridge");
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
        if (webView != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            webView.evaluateJavascript(
                "(function(){" +
                "  var m = document.querySelector('.modal-overlay.open');" +
                "  if (m) { closeModals(); return 'handled'; }" +
                "  if (document.body.classList.contains('drawer-open')) { toggleMobileDrawer(false); return 'handled'; }" +
                "  return 'exit';" +
                "})()",
                value -> {
                    if (value == null || !value.contains("handled")) {
                        MainActivity.super.onBackPressed();
                    }
                }
            );
        } else {
            super.onBackPressed();
        }
    }
}
