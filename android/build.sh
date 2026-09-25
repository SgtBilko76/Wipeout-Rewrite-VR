#!/bin/bash
# Builds a standalone APK for Meta Quest or PICO headsets (arm64, OpenXR).
# The native code is the same; only the Android manifest differs.
#
# Requirements: Android SDK with NDK, build-tools and a platform (API 32+),
# CMake, Ninja, Java (keytool). Set ANDROID_HOME if the SDK is not in
# ~/Android/Sdk. Usage:
#
#   android/build.sh [quest|pico] [install]
#
# builds build/android/wipeout-<headset>-<version>.apk (default: quest) and
# optionally installs it via adb.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HEADSET=quest
INSTALL=
for arg in "$@"; do
	case "$arg" in
		quest|pico) HEADSET="$arg" ;;
		install) INSTALL=1 ;;
		*) echo "Unknown argument: $arg" >&2; exit 1 ;;
	esac
done
MANIFEST="$ROOT/android/AndroidManifest.$HEADSET.xml"
OUT="$ROOT/build/android"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
BUILD_TOOLS="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLATFORM_JAR="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar"
MIN_SDK=29
TARGET_SDK=32
OPENXR_VERSION=1.1.63
OPENXR_DIR="$OUT/openxr_loader-$OPENXR_VERSION"
ABI=arm64-v8a
VERSION="$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$MANIFEST")"
APK="$OUT/wipeout-$HEADSET-$VERSION.apk"

mkdir -p "$OUT"

# OpenXR loader (headers + libopenxr_loader.so) from Khronos' Maven release
if [ ! -d "$OPENXR_DIR" ]; then
	AAR="$OUT/openxr_loader-$OPENXR_VERSION.aar"
	curl -fsSL -o "$AAR" "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_VERSION/openxr_loader_for_android-$OPENXR_VERSION.aar"
	mkdir -p "$OPENXR_DIR"
	unzip -q "$AAR" -d "$OPENXR_DIR"
fi

# Native library
cmake -S "$ROOT" -B "$OUT/cmake" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=$ABI \
	-DANDROID_PLATFORM=android-$MIN_SDK \
	-DCMAKE_BUILD_TYPE=Release \
	-DPLATFORM=QUEST \
	-DRENDERER=VR \
	-DOPENXR_LOADER_DIR="$OPENXR_DIR"
cmake --build "$OUT/cmake"

# APK
rm -rf "$OUT/apk" && mkdir -p "$OUT/apk/lib/$ABI"
cp "$OUT/cmake/libwipeout.so" "$OUT/apk/lib/$ABI/"
cp "$OPENXR_DIR/prefab/modules/openxr_loader/libs/android.$ABI/libopenxr_loader.so" "$OUT/apk/lib/$ABI/"

"$BUILD_TOOLS/aapt2" link -o "$OUT/unaligned.apk" \
	--manifest "$MANIFEST" \
	-I "$PLATFORM_JAR" \
	--min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK
(cd "$OUT/apk" && zip -qr "$OUT/unaligned.apk" lib)
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

KEYSTORE="$HOME/.android/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
	mkdir -p "$HOME/.android"
	keytool -genkeypair -keystore "$KEYSTORE" -storepass android -alias androiddebugkey \
		-keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
	--out "$APK" "$OUT/aligned.apk"
rm -f "$OUT/unaligned.apk" "$OUT/aligned.apk"

echo "Built $APK"

if [ -n "$INSTALL" ]; then
	adb install -r "$APK"
fi
