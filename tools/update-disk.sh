#!/bin/bash
set -e

cd "$(dirname "$0")/.."
FIRMWARE_DIR="$(pwd)"

# Default disk image path
DISK_IMG="${1:-$FIRMWARE_DIR/data/disk.img}"

if [ ! -f "$DISK_IMG" ]; then
    echo "Error: $DISK_IMG not found"
    echo "Usage: $0 [disk_image_path]"
    echo "Default: data/disk.img"
    exit 1
fi

for app in CydCtlApp WeatherApp WiFiApp OfficeLightsApp; do
    echo "Building $app..."
    (cd "$FIRMWARE_DIR/mac-app/$app" && ./build.sh)
done

echo "Updating disk.img..."
if command -v hmount >/dev/null 2>&1; then
    hmount "$DISK_IMG"
    hdel CydCtl 2>/dev/null || true
    hdel Weather 2>/dev/null || true
    hdel WiFi 2>/dev/null || true
    hdel OfficeLights 2>/dev/null || true
    hcopy -m "$FIRMWARE_DIR/mac-app/CydCtlApp/build/CydCtl.bin" :
    hcopy -m "$FIRMWARE_DIR/mac-app/WeatherApp/build/Weather.bin" :
    hcopy -m "$FIRMWARE_DIR/mac-app/WiFiApp/build/WiFi.bin" :
    hcopy -m "$FIRMWARE_DIR/mac-app/OfficeLightsApp/build/OfficeLights.bin" :
    humount
elif command -v hpmount >/dev/null 2>&1; then
    printf 'y\n' | hpmount "$DISK_IMG"
    hprm CydCtl 2>/dev/null || true
    hprm Weather 2>/dev/null || true
    hprm WiFi 2>/dev/null || true
    hprm OfficeLights 2>/dev/null || true
    hpcopy -m "$FIRMWARE_DIR/mac-app/CydCtlApp/build/CydCtl.bin" :
    hpcopy -m "$FIRMWARE_DIR/mac-app/WeatherApp/build/Weather.bin" :
    hpcopy -m "$FIRMWARE_DIR/mac-app/WiFiApp/build/WiFi.bin" :
    hpcopy -m "$FIRMWARE_DIR/mac-app/OfficeLightsApp/build/OfficeLights.bin" :
    hpumount
else
    echo "Error: no supported HFS utility found (hmount/hcopy or hpmount/hpcopy)"
    exit 1
fi

echo "Done."