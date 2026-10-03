#!/bin/sh
# Uppdatera kaffrostaren över nätet med PUSH: den har maskinen ansluter TILL
# roastern och skickar firmware-imagen.
#
#   sh tools/ota-push.sh 192.168.x.x            firmware (app-partitionen)
#   sh tools/ota-push.sh 192.168.x.x --dry-run  visa bara vad som skulle skickas
#
# Varför den här vägen och inte tools/ota-upload.sh (ArduinoOTA): ArduinoOTA
# kräver att ENHETEN öppnar en TCP-anslutning tillbaka till uppladdaren. I ett
# VLAN-delat hemmanät är nya anslutningar IoT -> LAN blockerade, så den
# anslutningen kommer aldrig fram och överföringen dör efter
# "Authenticating...OK". LAN -> IoT fungerar, så här vänder vi på riktningen i
# stället. Se include/ota_push.h och docs/firmware-notes.md.
#
# Token står inte här och kommer aldrig att stå här: den läses ur
# include/secrets.h (gitignored, 0600) och skickas som X-OTA-Token i headern.
# Är den tom står endpointen av i firmwaren och det här skriptet vägrar köra.
#
# Bygg först:  pio run
# (Ändringar i data/ måste fortfarande OTA:as med tools/ota-upload.sh <ip> fs,
#  den vägen använder espota-protokollet och rör inte app-partitionen.)
set -e
cd "$(dirname "$0")/.."

HOST="$1"
DRY_RUN=0
if [ "${2:-}" = "--dry-run" ]; then DRY_RUN=1; fi

if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-push.sh <ip-eller-host> [--dry-run]"
  echo "  exempel:   sh tools/ota-push.sh 192.168.2.x"
  exit 2
fi

IMAGE=".pio/build/esp32dev/firmware.bin"
if [ ! -f "$IMAGE" ]; then
  echo "FEL: $IMAGE finns inte - kör 'pio run' först."
  exit 1
fi

TOKEN=$(sed -n 's/^#define[[:space:]]*OTA_TOKEN[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$TOKEN" ]; then
  echo "FEL: OTA_TOKEN saknas i include/secrets.h."
  echo "      Push-OTA är avstängt i firmwaren - sätt en token där först."
  exit 1
fi

SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
echo "Mål:    http://$HOST/api/update (POST, app-partitionen)"
echo "Bild:   $IMAGE ($SIZE byte)"

if [ "$DRY_RUN" = "1" ]; then
  echo "Dry run - skickar ingenting. Röstningen av enheten:"
  curl -s -o /dev/null -w '  /api/status svarar HTTP %{http_code}\n' "http://$HOST/api/status" || true
  exit 0
fi

# Enheten svarar 200 först när hela imagen är skriven och verifierad, och
# startar om strax därefter. Därför timeout på anslutningen: en lyckad
# överföring kan se ut som ett avbrott på slutet.
CODE=$(curl -s -o /tmp/ota-push-response.$$ -w '%{http_code}' \
  --connect-timeout 10 --max-time 600 \
  -X POST --data-binary "@$IMAGE" \
  -H "Content-Type: application/octet-stream" \
  -H "X-OTA-Token: $TOKEN" \
  "http://$HOST/api/update" || true)

echo "Svar:   HTTP $CODE"
cat /tmp/ota-push-response.$$ 2>/dev/null || true
rm -f /tmp/ota-push-response.$$

case "$CODE" in
  200) echo "KLART: imagen skriven och verifierad - enheten startar om i den nya versionen." ;;
  401) echo "FEL: token stämmer inte (401). Kontrollera OTA_TOKEN i include/secrets.h och i firmwaren." ;;
  409) echo "FEL: enheten nekar just nu (409) - en rostning, manuell körning eller kylning är igång." ;;
  413) echo "FEL: imagen är större än OTA-slotten (413)." ;;
  503) echo "FEL: enheten har ingen OTA_TOKEN konfigurerad (503)." ;;
  000) echo "FEL: ingen kontakt med enheten. Kontrollera IP:t och att den är online." ;;
  *)   echo "FEL: oväntat svar (HTTP $CODE)." ;;
esac
