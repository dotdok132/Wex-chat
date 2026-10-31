#!/usr/bin/env bash
set -e

SDK="/home/dotdok/Android/Sdk"
BUILD_TOOLS="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"

NDK="${ANDROID_NDK_HOME:-/home/dotdok/.android/sdk/ndk/25.2.9519653}"
if [ ! -d "$NDK" ]; then
    NDK=$(find /home/dotdok/.android/sdk/ndk /home/dotdok/Android/Sdk/ndk -maxdepth 1 -mindepth 1 -type d 2>/dev/null | head -n 1)
fi
NDK_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
STRIP="$NDK_BIN/llvm-strip"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="$(cd "$ROOT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

echo "[*] Cleaning and preparing build directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/gen" "$BUILD_DIR/classes" "$BUILD_DIR/dex" \
         "$BUILD_DIR/lib/arm64-v8a" "$BUILD_DIR/lib/armeabi-v7a" "$BUILD_DIR/lib/x86_64"

C_SOURCES="$SRC_ROOT/vendor/monocypher/monocypher.c \
$SRC_ROOT/vendor/monocypher/monocypher-ed25519.c \
$SRC_ROOT/vendor/cjson/cJSON.c \
$SRC_ROOT/src/crypto/wex_crypto.c \
$SRC_ROOT/src/models/wex_models.c \
$SRC_ROOT/src/models/wex_config.c \
$SRC_ROOT/src/models/wex_permissions.c \
$SRC_ROOT/src/models/wex_actions.c \
$SRC_ROOT/src/net/wex_ws_server.c \
$SRC_ROOT/src/net/wex_p2p.c \
$SRC_ROOT/src/net/wex_nat.c \
$SRC_ROOT/src/net/wex_voice.c \
$SRC_ROOT/src/storage/wex_store.c \
$SRC_ROOT/src/storage/wex_persist.c \
$SRC_ROOT/src/storage/wex_blob.c \
$SRC_ROOT/src/dht/wex_dht.c \
$ROOT_DIR/app/src/main/jni/wex_jni.c"

echo "[*] Compiling native C99 Wex P2P Core (libwex.so) for ARM64..."
"$NDK_BIN/aarch64-linux-android24-clang" -O2 -fPIC -shared \
    -I "$SRC_ROOT/include" -I "$SRC_ROOT/vendor/monocypher" -I "$SRC_ROOT/vendor/cjson" \
    $C_SOURCES -llog -o "$BUILD_DIR/lib/arm64-v8a/libwex.so"
"$STRIP" "$BUILD_DIR/lib/arm64-v8a/libwex.so"

echo "[*] Compiling native C99 Wex P2P Core (libwex.so) for ARMv7..."
"$NDK_BIN/armv7a-linux-androideabi24-clang" -O2 -fPIC -shared \
    -I "$SRC_ROOT/include" -I "$SRC_ROOT/vendor/monocypher" -I "$SRC_ROOT/vendor/cjson" \
    $C_SOURCES -llog -o "$BUILD_DIR/lib/armeabi-v7a/libwex.so"
"$STRIP" "$BUILD_DIR/lib/armeabi-v7a/libwex.so"

echo "[*] Compiling native C99 Wex P2P Core (libwex.so) for x86_64..."
"$NDK_BIN/x86_64-linux-android24-clang" -O2 -fPIC -shared \
    -I "$SRC_ROOT/include" -I "$SRC_ROOT/vendor/monocypher" -I "$SRC_ROOT/vendor/cjson" \
    $C_SOURCES -llog -o "$BUILD_DIR/lib/x86_64/libwex.so"
"$STRIP" "$BUILD_DIR/lib/x86_64/libwex.so"

echo "[*] Synchronizing web UI assets (HTML, scripts, styles, QR bundles) into Android assets..."
mkdir -p "$ROOT_DIR/app/src/main/assets"
if [ -d "$SRC_ROOT/web" ]; then
    cp -rf "$SRC_ROOT/web/"* "$ROOT_DIR/app/src/main/assets/"
fi

echo "[*] Compiling Android resources with AAPT2..."
"$AAPT2" compile --dir "$ROOT_DIR/app/src/main/res" -o "$BUILD_DIR/compiled_res.zip"

echo "[*] Linking resources and packaging assets..."
"$AAPT2" link \
    -I "$PLATFORM" \
    -A "$ROOT_DIR/app/src/main/assets" \
    --manifest "$ROOT_DIR/app/src/main/AndroidManifest.xml" \
    --java "$BUILD_DIR/gen" \
    --auto-add-overlay \
    -o "$BUILD_DIR/unaligned.apk" \
    "$BUILD_DIR/compiled_res.zip"

echo "[*] Compiling Java source files..."
javac -g -source 8 -target 8 -Xlint:-options \
    -cp "$PLATFORM" \
    -d "$BUILD_DIR/classes" \
    "$BUILD_DIR/gen/net/wexchat/R.java" \
    "$ROOT_DIR/app/src/main/java/net/wexchat/"*.java

echo "[*] Converting bytecode to Dalvik DEX via D8..."
"$D8" --lib "$PLATFORM" --output "$BUILD_DIR/dex" "$BUILD_DIR/classes/net/wexchat/"*.class

echo "[*] Packaging classes.dex and native libwex.so into APK package..."
cd "$BUILD_DIR/dex"
jar -uf "$BUILD_DIR/unaligned.apk" classes.dex
cd "$BUILD_DIR"
jar -uf "$BUILD_DIR/unaligned.apk" lib/arm64-v8a/libwex.so lib/armeabi-v7a/libwex.so lib/x86_64/libwex.so
cd "$ROOT_DIR"

echo "[*] Aligning APK with zipalign..."
"$ZIPALIGN" -f 4 "$BUILD_DIR/unaligned.apk" "$BUILD_DIR/wex-chat-uncompressed.apk"

echo "[*] Generating debug keystore if needed..."
KEYSTORE="$BUILD_DIR/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkey -v -keystore "$KEYSTORE" \
        -alias androiddebugkey -storepass android -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=WexChat, OU=Dev, O=Wex, L=Earth, S=Cyberspace, C=UN"
fi

echo "[*] Signing APK via apksigner..."
"$APKSIGNER" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias androiddebugkey \
    --ks-pass pass:android \
    --key-pass pass:android \
    --out "$ROOT_DIR/wex-chat.apk" \
    "$BUILD_DIR/wex-chat-uncompressed.apk"

echo "[*] Verifying signed APK..."
"$APKSIGNER" verify "$ROOT_DIR/wex-chat.apk"

echo ""
echo "============================================================"
echo "[SUCCESS] Wex-Chat Android APK generated successfully!"
echo "Target APK: $ROOT_DIR/wex-chat.apk"
ls -lh "$ROOT_DIR/wex-chat.apk"
echo "============================================================"
