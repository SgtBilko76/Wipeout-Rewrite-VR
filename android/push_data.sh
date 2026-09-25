#!/bin/bash
# Copies the game data from wipeout/ to the Quest. The data is sent as one
# tar file which the game unpacks on its next start; directories created by
# adb would not be readable by the game.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGE=online.portroyale.wipeoutrewritevr
DEST="/sdcard/Android/data/$PACKAGE/files"
TAR="$ROOT/build/android/wipeout.tar"

if [ ! -d "$ROOT/wipeout/track01" ]; then
	echo "No game data in $ROOT/wipeout/ (see README)" >&2
	exit 1
fi

mkdir -p "$(dirname "$TAR")"
tar --format=ustar -cf "$TAR" -C "$ROOT" --exclude=game_data_goes_here.txt wipeout

# Start the game once so that it creates its data dir (owned by the app)
if ! adb shell "[ -d $DEST ]"; then
	adb shell am start -n "$PACKAGE/android.app.NativeActivity" > /dev/null
	for i in $(seq 1 20); do
		adb shell "[ -d $DEST ]" && break
		sleep 0.5
	done
	adb shell am force-stop "$PACKAGE"
fi

adb push "$TAR" "$DEST/wipeout.tar"
echo "Pushed game data; it will be unpacked the next time the game starts"
