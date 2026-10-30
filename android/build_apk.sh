#!/usr/bin/env bash
set -e

SDK="/home/dotdok/Android/Sdk"
BUILD_TOOLS="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"

echo "[*] Cleaning and preparing build directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/gen" "$BUILD_DIR/classes" "$BUILD_DIR/dex"

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
javac -g -source 8 -target 8 \
    -cp "$PLATFORM" \
    -d "$BUILD_DIR/classes" \
    "$BUILD_DIR/gen/net/wexchat/R.java" \
    "$ROOT_DIR/app/src/main/java/net/wexchat/MainActivity.java"

echo "[*] Converting bytecode to Dalvik DEX via D8..."
"$D8" --lib "$PLATFORM" --output "$BUILD_DIR/dex" "$BUILD_DIR/classes/net/wexchat/"*.class

echo "[*] Adding classes.dex to APK package..."
cd "$BUILD_DIR/dex"
jar -uf "$BUILD_DIR/unaligned.apk" classes.dex
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
