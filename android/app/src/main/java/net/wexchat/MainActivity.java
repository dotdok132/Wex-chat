package net.wexchat;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.MediaStore;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.JsPromptResult;
import android.webkit.JsResult;
import android.webkit.PermissionRequest;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private WebView webView;
    private ValueCallback<Uri[]> filePathCallback;
    private PermissionRequest pendingPermissionRequest;

    public static final int REQUEST_CODE_STARTUP_PERMISSIONS = 101;
    public static final int REQUEST_CODE_WEB_PERMISSIONS = 102;
    public static final int REQUEST_CODE_FILE_CHOOSER = 1002;

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

        @JavascriptInterface
        public String getDeviceName() {
            return (Build.MODEL != null && !Build.MODEL.trim().isEmpty()) ? Build.MODEL.trim() : "Android";
        }
    }

    public static class WexChromeClient extends WebChromeClient {
        private final MainActivity activity;

        public WexChromeClient(MainActivity activity) {
            this.activity = activity;
        }

        @Override
        public void onPermissionRequest(final PermissionRequest request) {
            if (activity != null) {
                activity.runOnUiThread(() -> activity.handlePermissionRequest(request));
            }
        }

        @Override
        public void onPermissionRequestCanceled(PermissionRequest request) {
            if (activity != null) {
                activity.handlePermissionRequestCanceled(request);
            }
        }

        @Override
        public boolean onShowFileChooser(WebView webView, ValueCallback<Uri[]> callback, FileChooserParams params) {
            if (activity != null) {
                return activity.handleShowFileChooser(callback, params);
            }
            return false;
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

    public void handlePermissionRequest(PermissionRequest request) {
        if (request == null) return;
        String[] resources = request.getResources();
        if (resources == null || resources.length == 0) {
            request.deny();
            return;
        }

        List<String> missingPermissions = new ArrayList<String>();
        for (String r : resources) {
            if (PermissionRequest.RESOURCE_VIDEO_CAPTURE.equals(r)) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M &&
                    checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                    missingPermissions.add(android.Manifest.permission.CAMERA);
                }
            } else if (PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(r)) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M &&
                    checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                    missingPermissions.add(android.Manifest.permission.RECORD_AUDIO);
                }
            }
        }

        if (missingPermissions.isEmpty()) {
            request.grant(resources);
        } else {
            if (pendingPermissionRequest != null) {
                pendingPermissionRequest.deny();
            }
            pendingPermissionRequest = request;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                requestPermissions(missingPermissions.toArray(new String[0]), REQUEST_CODE_WEB_PERMISSIONS);
            } else {
                request.grant(resources);
            }
        }
    }

    public void handlePermissionRequestCanceled(PermissionRequest request) {
        if (pendingPermissionRequest == request) {
            pendingPermissionRequest = null;
        }
    }

    public void handlePendingPermissionResult(PermissionRequest req) {
        if (req == null) return;
        boolean cameraOk = (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) ||
            (checkSelfPermission(android.Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED);
        boolean audioOk = (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) ||
            (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED);

        List<String> grantable = new ArrayList<String>();
        for (String r : req.getResources()) {
            if (PermissionRequest.RESOURCE_VIDEO_CAPTURE.equals(r)) {
                if (cameraOk) grantable.add(r);
            } else if (PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(r)) {
                if (audioOk) grantable.add(r);
            } else {
                grantable.add(r);
            }
        }

        if (!grantable.isEmpty()) {
            req.grant(grantable.toArray(new String[0]));
        } else {
            req.deny();
        }
    }

    public boolean handleShowFileChooser(ValueCallback<Uri[]> callback, WebChromeClient.FileChooserParams params) {
        if (filePathCallback != null) {
            filePathCallback.onReceiveValue(null);
            filePathCallback = null;
        }
        filePathCallback = callback;

        Intent chooserIntent = null;
        try {
            if (params != null) {
                chooserIntent = params.createIntent();
            }
        } catch (Exception ignored) {
        }

        if (chooserIntent == null) {
            chooserIntent = new Intent(Intent.ACTION_GET_CONTENT);
            chooserIntent.addCategory(Intent.CATEGORY_OPENABLE);
            chooserIntent.setType("image/*");
        } else {
            if (params != null) {
                String[] types = params.getAcceptTypes();
                if (types != null && types.length > 0 && !types[0].isEmpty()) {
                    chooserIntent.setType(types[0]);
                }
            }
        }

        Intent galleryIntent = new Intent(Intent.ACTION_PICK, MediaStore.Images.Media.EXTERNAL_CONTENT_URI);
        galleryIntent.setType("image/*");

        Intent targetIntent = Intent.createChooser(chooserIntent, "Выбрать QR-код из галереи");
        targetIntent.putExtra(Intent.EXTRA_INITIAL_INTENTS, new Intent[] { galleryIntent });

        try {
            startActivityForResult(targetIntent, REQUEST_CODE_FILE_CHOOSER);
            return true;
        } catch (Exception e) {
            try {
                startActivityForResult(chooserIntent, REQUEST_CODE_FILE_CHOOSER);
                return true;
            } catch (Exception ex) {
                if (filePathCallback != null) {
                    filePathCallback.onReceiveValue(null);
                    filePathCallback = null;
                }
                return false;
            }
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            Window window = getWindow();
            window.clearFlags(WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS);
            window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
            window.setStatusBarColor(Color.parseColor("#181622"));
            window.setNavigationBarColor(Color.parseColor("#181622"));
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
        webView.setBackgroundColor(Color.parseColor("#181622"));
        webView.setLayerType(View.LAYER_TYPE_HARDWARE, null);
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
        webView.setWebChromeClient(new WexChromeClient(this));

        // Check runtime permissions for camera, mic, and push notifications
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            List<String> perms = new ArrayList<String>();
            if (checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                perms.add(android.Manifest.permission.CAMERA);
            }
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                perms.add(android.Manifest.permission.RECORD_AUDIO);
            }
            if (Build.VERSION.SDK_INT >= 33) {
                if (checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED) {
                    perms.add("android.permission.POST_NOTIFICATIONS");
                }
            }
            if (!perms.isEmpty()) {
                requestPermissions(perms.toArray(new String[0]), REQUEST_CODE_STARTUP_PERMISSIONS);
            }
        }

        webView.loadUrl("file:///android_asset/index.html");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CODE_FILE_CHOOSER) {
            if (filePathCallback == null) {
                return;
            }
            Uri[] results = null;
            if (resultCode == Activity.RESULT_OK) {
                if (data != null) {
                    try {
                        results = WebChromeClient.FileChooserParams.parseResult(resultCode, data);
                    } catch (Exception ignored) {
                    }
                    if (results == null) {
                        if (data.getData() != null) {
                            results = new Uri[] { data.getData() };
                        } else if (data.getClipData() != null) {
                            ClipData clip = data.getClipData();
                            int count = clip.getItemCount();
                            if (count > 0) {
                                results = new Uri[count];
                                for (int i = 0; i < count; i++) {
                                    results[i] = clip.getItemAt(i).getUri();
                                }
                            }
                        }
                    }
                }
            }
            filePathCallback.onReceiveValue(results);
            filePathCallback = null;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_CODE_WEB_PERMISSIONS || requestCode == REQUEST_CODE_STARTUP_PERMISSIONS) {
            if (pendingPermissionRequest != null) {
                PermissionRequest req = pendingPermissionRequest;
                pendingPermissionRequest = null;
                handlePendingPermissionResult(req);
            }
        }
    }

    @Override
    protected void onDestroy() {
        if (filePathCallback != null) {
            filePathCallback.onReceiveValue(null);
            filePathCallback = null;
        }
        if (pendingPermissionRequest != null) {
            pendingPermissionRequest.deny();
            pendingPermissionRequest = null;
        }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (webView != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            webView.evaluateJavascript(
                "(function(){" +
                "  var m = document.querySelector('.modal-overlay.open');" +
                "  if (m) { if (typeof closeModals === 'function') closeModals(); return 'handled'; }" +
                "  if (document.body.classList.contains('chat-active')) { if (typeof closeActiveChat === 'function') closeActiveChat(); else document.body.classList.remove('chat-active'); return 'handled'; }" +
                "  if (document.body.classList.contains('drawer-open')) { if (typeof toggleMobileDrawer === 'function') toggleMobileDrawer(false); return 'handled'; }" +
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
