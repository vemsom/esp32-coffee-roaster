#!/bin/sh
# Uppdatera kaffrostaren över nätet med PUSH: den här maskinen ansluter TILL
# roastern och skickar imagen.
#
#   sh tools/ota-push.sh 192.168.x.x            firmware (app-partitionen)
#   sh tools/ota-push.sh 192.168.x.x fs         filsystemet (webb-UI:t)
#   sh tools/ota-push.sh 192.168.x.x --dry-run  visa bara vad som skulle skickas
#   sh tools/ota-push.sh 192.168.x.x fs --dry-run
#
# Varför den här vägen och inte tools/ota-upload.sh (ArduinoOTA): ArduinoOTA
# kräver att ENHETEN öppnar en TCP-anslutning tillbaka till uppladdaren. I ett
# segmenterat nät är nya anslutningar IoT -> LAN blockerade, så den
# anslutningen kommer aldrig fram och överföringen dör efter
# "Authenticating...OK". LAN -> IoT fungerar, så här vänder vi på riktningen i
# stället. Se include/ota_push.h och docs/firmware-notes.md.
#
# Två partitioner, två routes, samma skydd:
#   app  -> POST /api/update       (Update.begin(total, U_FLASH))
#   fs   -> POST /api/update/fs    (Update.begin(total, U_SPIFFS))
# Filsystemsbilden skrivs till partitionen som LittleFS ligger på, så enheten
# avmonterar filsystemet innan första byten och monterar tillbaka det efteråt
# (och profiltyendpointarna svarar 503 under tiden). Rutten avgör partitionen -
# en app-bild kan inte hamna i filsystemspartitionen genom att skickas till fel
# sökväg, den avvisas av storlekskontrollen eller skrivs till fel partition.
#
# Token står inte här och kommer aldrig att stå här: den läses ur
# include/secrets.h (gitignored, 0600) och skickas som X-OTA-Token i headern.
# Är den tom står endpointen av i firmwaren (503) och det här skriptet vägrar
# köra.
#
# Kontrollsumman räknas ut här och skickas som X-OTA-SHA256. Enheten hashar
# imagen själv medan den skriver och vägrar starta om något inte stämmer - den
# token som färdas i klartext över Wi-Fi är alltså inte det som skyddar
# innehållet. Skriptet räknar med sha256sum; saknas det avbryter vi hellre än
# att skicka en bild utan verifiering.
#
# Enheten accepterar bara anrop från OTA_ALLOWED_CLIENT_IP (den här maskinens adress) -
# en giltig token från en annan adress i IoT-nätet nekas. Vilken adress
# enheten väntar sig står i include/secrets.h; kör skriptet från den maskinen.
#
# Bygg först (OTA-environ, samma som skriptet skickar):
#   pio run -e esp32-ota                 # app: .pio/build/esp32-ota/firmware.bin
#   pio run -e esp32-ota -t buildfs      # fs:  .pio/build/esp32-ota/littlefs.bin
#
# Den här maskinen kör BT-only (ET-kanalen är död hårdvara) och byggs i miljön
# esp32-ota-bt - se platformio.ini och docs/firmware-notes.md, "Maskinens
# byggläge". Skriptet tar då bilden ur den miljöns byggkatalog i stället:
#   OTA_ENV=esp32-ota-bt pio run -e esp32-ota-bt        # app
#   OTA_ENV=esp32-ota-bt sh tools/ota-push.sh <ip>
# Utan OTA_ENV skickas esp32-ota, precis som förut.
#
# Vilken bild som skickas: .pio/build/$OTA_ENV/... - OTA-environ.
# esp32dev-imagen duger INTE, även om den är lika stor och byggs ur samma
# källor: build-id:n skiljer sig (se fw_build_id.py - FW_BUILD_TIME är en
# tidsstämpel per bygge), och enheten svarar 403 på en bild vars X-OTA-SHA256
# inte stämmer med innehållet.
#
# Efter en lyckad fs-push kontrolleras det som faktiskt betyder något: att
# enheten skickar ut en sida av SAMMA storlek som data/index.html och att
# varningspillen finns i den. Utan den kontrollen går det inte att skilja en
# push som landade från en som tystnade.
set -e
cd "$(dirname "$0")/.."

# Miljön bilden byggdes i. Default esp32-ota (oförändrat); maskinens BT-only-
# läge heter esp32-ota-bt och väljs med OTA_ENV. Katalogen följer miljön, så en
# bild från "fel" läge kan inte smyga med av misstag.
ENV_NAME="${OTA_ENV:-esp32-ota}"

HOST=""
TARGET="app"
DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    fs|filesystem|littlefs) TARGET="fs" ;;
    app|firmware)           TARGET="app" ;;
    --dry-run)              DRY_RUN=1 ;;
    -*)                     echo "Okänd flagga: $arg" >&2; exit 2 ;;
    *)                      HOST="$arg" ;;
  esac
done

if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-push.sh <ip-eller-host> [app|fs] [--dry-run]"
  echo "  exempel:   sh tools/ota-push.sh <rostarens-ip>"
  echo "             sh tools/ota-push.sh <rostarens-ip> fs"
  exit 2
fi

if [ "$TARGET" = "fs" ]; then
  IMAGE=".pio/build/$ENV_NAME/littlefs.bin"
  ROUTE="/api/update/fs"
  BUILD_HINT="pio run -e $ENV_NAME -t buildfs"
  PARTITION_DESC="filsystemspartitionen (LittleFS / webb-UI:t)"
  MODE="fs"
else
  IMAGE=".pio/build/$ENV_NAME/firmware.bin"
  ROUTE="/api/update"
  BUILD_HINT="pio run -e $ENV_NAME"
  PARTITION_DESC="app-partitionen"
  MODE="app"
fi

if [ ! -f "$IMAGE" ]; then
  echo "FEL: $IMAGE finns inte - kör '$BUILD_HINT' först."
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
echo "Mål:    http://$HOST$ROUTE (POST, $PARTITION_DESC)"
echo "Bild:   $IMAGE ($SIZE byte)"
echo "SHA256: $SHA"

# Lokal förväntan på webbsidan, för kontrollen efter en fs-push.
LOCAL_PAGE="data/index.html"
LOCAL_PAGE_SIZE=0
if [ "$MODE" = "fs" ] && [ -f "$LOCAL_PAGE" ]; then
  LOCAL_PAGE_SIZE=$(wc -c < "$LOCAL_PAGE" | tr -d ' ')
fi

# Sidans storlek som enheten faktiskt skickar ut just nu. Det här är talet som
# avgör om den förra fs-pushen landade - en app-push kan vara ny medan sidan
# fortfarande är den gamla filsystemsbilden.
served_page_size() {
  curl -s --max-time 15 "http://$HOST/" 2>/dev/null | wc -c | tr -d ' '
}

report_status() {
  # Bara läskontroll: fw-version och build-id är det som bevisar att rätt bild
  # kör, och etDisabled/safetyFault hör till samma bild.
  echo "Enheten svarar:"
  curl -s --max-time 10 "http://$HOST/api/status" 2>/dev/null || true
  echo ""
}

if [ "$DRY_RUN" = "1" ]; then
  echo "Dry run - skickar ingenting. Röstningen av enheten:"
  curl -s -o /dev/null -w '  /api/status svarar HTTP %{http_code}\n' "http://$HOST/api/status" || true
  # GET på update-routen är probe-routen: 405 = rätt klient (den här maskinen är
  # den enda som får veta att endpointen finns), 404 = enheten känner inte igen
  # oss, eller kör firmware utan push-OTA.
  curl -s -o /dev/null -w "  $ROUTE (GET) svarar HTTP %{http_code}\n" \
    -H "X-OTA-Token: $TOKEN" -H "X-OTA-SHA256: $SHA" "http://$HOST$ROUTE" || true
  echo "  enhetens sida just nu: $(served_page_size) byte"
  if [ "$MODE" = "fs" ]; then
    echo "  $LOCAL_PAGE lokalt:    $LOCAL_PAGE_SIZE byte"
  fi
  exit 0
fi

if [ "$MODE" = "fs" ] && [ "$LOCAL_PAGE_SIZE" != "0" ]; then
  echo "Sidan enheten skickar ut FÖRE pushen: $(served_page_size) byte"
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
  "http://$HOST$ROUTE") || CODE=000

echo "Svar:   HTTP $CODE"
cat /tmp/ota-push-response.$$ 2>/dev/null || true
rm -f /tmp/ota-push-response.$$

wait_for_device() {
  echo "Väntar på att enheten ska svara igen (upp till 40 s)..."
  TRIES=0
  while [ "$TRIES" -lt 20 ]; do
    sleep 2
    TRIES=$((TRIES + 1))
    BACK=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
      "http://$HOST/api/status" 2>/dev/null) || BACK=000
    if [ "$BACK" = "200" ]; then
      echo "Enheten svarar igen efter ${TRIES} forsok."
      return 0
    fi
  done
  echo "FEL: ingen kontakt med enheten och den kom inte tillbaka inom 40 s."
  echo "      Kontrollera IP:t, att den är online, och den seriella loggen."
  return 1
}

if [ "$CODE" = "000" ]; then
  # Antingen står enheten still (kabel/lösen) eller så hann den starta om
  # mitt i svaret. Den andra möjligheten är den vanliga: vänta in den och
  # döm på om den kommer tillbaka, inte på curl-exitkoden.
  wait_for_device || exit 1
elif [ "$CODE" = "200" ]; then
  echo "KLART: imagen skriven och verifierad - enheten startar om."
  wait_for_device || exit 1
else
  case "$CODE" in
    400) echo "FEL: enheten avvisade anropet (400) - för liten eller saknad" ;;
    403) echo "FEL: kontrollsumman stämde inte (403). Imagen skrevs aldrig klart och"
         echo "      enheten kör kvar sin gamla bild. Bygg om och försök igen." ;;
    404) echo "FEL: enheten svarar 404. Antingen känner den inte igen vår adress"
         echo "      (OTA_ALLOWED_CLIENT_IP i include/secrets.h) eller kör den firmware"
         echo "      utan push-OTA (fs-routen finns först i 0.10.0 - en 0.9.0-enhet"
         echo "      svarar 404 på /api/update/fs). Kör skriptet från den maskinen."
         if [ "$MODE" = "fs" ]; then
           echo "      Skicka app-imagen först: sh tools/ota-push.sh $HOST"
         fi ;;
    405) echo "FEL: enheten svarar 405 (GET på POST-routen) - använd POST-vägen." ;;
    409) echo "FEL: enheten nekar just nu (409) - en rostning, manuell körning, kylning,"
         echo "      ett varmt element eller en annan överföring är igång. Vänta." ;;
    413) echo "FEL: imagen är större än målpartitionen (413)." ;;
    500) echo "FEL: enheten kunde inte skriva imagen (500). Inget startades om." ;;
    503) echo "FEL: enheten har ingen OTA_TOKEN konfigurerad (503) - endpointen är av." ;;
    *)   echo "FEL: oväntat svar (HTTP $CODE)." ;;
  esac
  exit 1
fi

report_status

if [ "$MODE" = "fs" ]; then
  # Det som avgör om fs-pushen landade: samma sidstorlek som filen i repot, och
  # varningstexten i den. Enheten kan svara 200 och ändå skicka ut den gamla
  # sidan - det var precis vad som hände när filsystemet aldrig kom in.
  SERVED=$(served_page_size)
  echo "Sidan enheten skickar ut EFTER pushen: $SERVED byte ($LOCAL_PAGE lokalt: $LOCAL_PAGE_SIZE byte)"
  if [ "$SERVED" != "$LOCAL_PAGE_SIZE" ]; then
    echo "FEL: storleken stämmer inte - filsystemet är inte den nya sidan."
    echo "      Enheten kör en app som kan ta emot fs-push, men partitionen"
    echo "      innehåller en annan (äldre) filsystemsbild. Se docs/firmware-notes.md."
    exit 1
  fi
  PAGE=$(curl -s --max-time 15 "http://$HOST/" 2>/dev/null || true)
  MISSING=""
  case "$PAGE" in *"ET AVSTÄNGD"*) ;; *) MISSING="$MISSING 'ET AVSTÄNGD'" ;; esac
  case "$PAGE" in *"ET DISABLED"*) ;; *) MISSING="$MISSING 'ET DISABLED'" ;; esac
  if [ -n "$MISSING" ]; then
    echo "FEL: sidan är rätt storlek men pill-texten saknas:$MISSING"
    exit 1
  fi
  echo "KLART: filsystemet är den nya sidan ($SERVED byte, varningspillen finns i båda språken)."
else
  echo "KLART: app-imagen är inne. Kontrollera 'fw' och 'build' ovan mot"
  echo "        git rev-parse --short HEAD på den här maskinen."
fi
