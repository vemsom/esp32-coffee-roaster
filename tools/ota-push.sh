#!/bin/sh
# Uppdatera kaffrostaren över nätet med PUSH: den här maskinen ansluter TILL
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
# Kontrollsumman räknas ut här och skickas som X-OTA-SHA256. Enheten hashar
# imagen själv medan den skriver och vägrar starta om något inte stämmer - den
# token som färdas i klartext över Wi-Fi är alltså inte det som skyddar
# innehållet. Skriptet räknar med sha256sum; saknas det avbryter vi hellre än
# att skicka en bild utan verifiering.
#
# Enheten accepterar bara anrop från OTA_ALLOWED_CLIENT_IP (serverns adress) -
# en giltig token från en annan adress i IoT-nätet nekas. Vilken adress
# enheten väntar sig står i include/secrets.h; kör skriptet från den maskinen.
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

if ! command -v sha256sum >/dev/null 2>&1; then
  echo "FEL: sha256sum saknas. Utan kontrollsumma verifierar enheten inte"
  echo "      imagen, och då skickar vi hellre ingenting."
  exit 1
fi
SHA=$(sha256sum "$IMAGE" | awk '{print $1}')

SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
echo "Mål:    http://$HOST/api/update (POST, app-partitionen)"
echo "Bild:   $IMAGE ($SIZE byte)"
echo "SHA256: $SHA"

if [ "$DRY_RUN" = "1" ]; then
  echo "Dry run - skickar ingenting. Röstningen av enheten:"
  curl -s -o /dev/null -w '  /api/status svarar HTTP %{http_code}\n' "http://$HOST/api/status" || true
  # GET på /api/update är probe-routen: 405 = rätt klient (den här maskinen är
  # den enda som får veta att endpointen finns), 404 = enheten känner inte igen
  # oss, eller kör firmware utan push-OTA.
  curl -s -o /dev/null -w '  /api/update (GET) svarar HTTP %{http_code}\n' \
    -H "X-OTA-Token: $TOKEN" -H "X-OTA-SHA256: $SHA" "http://$HOST/api/update" || true
  exit 0
fi

# Enheten svarar 200 först när hela imagen är skriven och verifierad, och
# startar om strax därefter. Därför timeout på anslutningen: en lyckad
# överföring kan se ut som ett avbrott på slutet.
#
# curl kan alltså returnera 000 på en anslutning som enheten stängde EFTER att
# den svarat - och med `set -e` dör skriptet då utan att säga vad som hände.
# Enheten är borta en stund efter en lyckad push, så ett 000 får inte tolkas
# som "ingen kontakt" förrän vi försökt igen: statuskoden slås upp efteråt
# genom att fråga enheten om den är tillbaka.
CODE=$(curl -s -o /tmp/ota-push-response.$$ -w '%{http_code}' \
  --connect-timeout 10 --max-time 600 \
  -X POST --data-binary "@$IMAGE" \
  -H "Content-Type: application/octet-stream" \
  -H "X-OTA-Token: $TOKEN" \
  -H "X-OTA-SHA256: $SHA" \
  "http://$HOST/api/update") || CODE=000

echo "Svar:   HTTP $CODE"
cat /tmp/ota-push-response.$$ 2>/dev/null || true
rm -f /tmp/ota-push-response.$$

if [ "$CODE" = "000" ]; then
  # Antingen står enheten still (kabel/lösen) eller så hann den starta om
  # mitt i svaret. Den andra möjligheten är den vanliga: vänta in den och
  # döm på om den kommer tillbaka, inte på curl-exitkoden.
  echo "Ingen statusrad - väntar på att enheten ska svara igen (upp till 40 s)..."
  TRIES=0
  while [ "$TRIES" -lt 20 ]; do
    sleep 2
    TRIES=$((TRIES + 1))
    BACK=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
      "http://$HOST/api/status" 2>/dev/null) || BACK=000
    if [ "$BACK" = "200" ]; then
      echo "KLART: enheten svarar igen efter ${TRIES} forsok - den startade om i den nya imagen."
      exit 0
    fi
  done
  echo "FEL: ingen kontakt med enheten och den kom inte tillbaka inom 40 s."
  echo "      Kontrollera IP:t, att den är online, och den seriella loggen."
  exit 1
fi

case "$CODE" in
  200) echo "KLART: imagen skriven och verifierad - enheten startar om i den nya versionen." ;;
  403) echo "FEL: kontrollsumman stämde inte (403). Imagen skrevs aldrig klart och"
       echo "      enheten kör kvar sin gamla firmware. Kör 'pio run' och försök igen." ;;
  404) echo "FEL: enheten svarar 404. Antingen känner den inte igen vår adress"
       echo "      (OTA_ALLOWED_CLIENT_IP i include/secrets.h) eller kör den firmware"
       echo "      utan push-OTA. Kör skriptet från servern 192.168.1.x." ;;
  405) echo "FEL: enheten svarar 405 (GET på POST-routen) - använd POST-vägen." ;;
  409) echo "FEL: enheten nekar just nu (409) - en rostning, manuell körning, kylning"
       echo "      eller ett varmt element är igång. Vänta tills den är stilla." ;;
  413) echo "FEL: imagen är större än OTA-slotten (413)." ;;
  500) echo "FEL: enheten kunde inte skriva imagen (500). Inget startades om." ;;
  503) echo "FEL: enheten har ingen OTA_TOKEN konfigurerad (503)." ;;
  *)   echo "FEL: oväntat svar (HTTP $CODE)." ;;
esac
