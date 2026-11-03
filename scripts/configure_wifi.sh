#!/usr/bin/env bash
# Build firmware with local Wi-Fi credentials and server URL.
#
#   1. cp wifi.env.example wifi.env   (wifi.env is gitignored)
#   2. edit wifi.env
#   3. ./scripts/configure_wifi.sh
#
# Re-running is safe: it regenerates sdkconfig.local and sdkconfig from defaults.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")/firmware"
env_file="$here/wifi.env"

if [[ ! -f "$env_file" ]]; then
    echo "missing $env_file - copy wifi.env.example and fill it in" >&2
    exit 1
fi

# shellcheck disable=SC1090
source "$env_file"
: "${WIFI_SSID:?set WIFI_SSID in scripts/wifi.env}"
: "${SERVER_URL:?set SERVER_URL in scripts/wifi.env, e.g. ws://192.168.1.50:8000/ws/audio}"

cat > "$root/sdkconfig.local" <<CONFIG
CONFIG_PLAUD_WIFI_SSID="$WIFI_SSID"
CONFIG_PLAUD_WIFI_PASSWORD="$WIFI_PASSWORD"
CONFIG_PLAUD_SERVER_URL="$SERVER_URL"
CONFIG

cd "$root"
rm -f sdkconfig
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local"
idf.py reconfigure
"$here/patch_bsp_display.sh"
idf.py build
echo "built with SSID '$WIFI_SSID' -> $SERVER_URL"
