#!/bin/sh
# Uppdatera kaffrostaren över nätet med PUSH: den här maskinen ansluter TILL
# roastern och skickar imagen.
#
#   sh tools/ota-push.sh 192.168.x.x            firmware (app-partitionen)
#   sh tools/ota-push.sh 192.168.x.x fs         filsystemet (webb-UI:t)
#   sh tools/ota-push.sh 192.168.x.x --dry-run  visa bara vad som skulle skickas
#   sh tools/ota-push.sh 192.168.x.x fs --dry-run
#   sh tools/ota-push.sh 192.168.x.x fs --no-profile-backup
#                                               fs-push UTAN profilbackup
#
# PROFILBACKUP VID FS-PUSH. Filsystemsbilden innehåller bara webb-UI:t
# (data/index.html -> littlefs.bin) och /api/update/fs ersätter HELA
# LittleFS-partitionen. Allt som ligger i /profiles raderas alltså vid varje
# fs-push - det har kostat rosteriprogram två gånger. Skriptet läser därför ut
# alla profiler (GET /api/profiles + GET /api/profile?name=...) till en
# temporär katalog INNAN pushen, och skickar tillbaka dem (POST /api/profiles)
# när enheten svarar igen. Misslyckas utläsningen avbryts pushen - en push som
# raderar profilerna utan att ha en kopia är precis det som inte får hända.
# --no-profile-backup finns för den som medvetet vill hoppa över steget (t.ex.
# en enhet utan profiler, eller en felsökning där man vill se partitionen rå).
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
PROFILE_BACKUP=1
for arg in "$@"; do
  case "$arg" in
    fs|filesystem|littlefs) TARGET="fs" ;;
    app|firmware)           TARGET="app" ;;
    --dry-run)              DRY_RUN=1 ;;
    --no-profile-backup)    PROFILE_BACKUP=0 ;;
    -*)                     echo "Okänd flagga: $arg" >&2; exit 2 ;;
    *)                      HOST="$arg" ;;
  esac
done

if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-push.sh <ip-eller-host> [app|fs] [--dry-run] [--no-profile-backup]"
  echo "  exempel:   sh tools/ota-push.sh <rostarens-ip>"
  echo "             sh tools/ota-push.sh <rostarens-ip> fs"
  echo "             sh tools/ota-push.sh <rostarens-ip> fs --no-profile-backup"
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

# ---------------------------------------------------------- profilbackup ---
# /api/update/fs ersätter hela LittleFS-partitionen med en bild som bara
# innehåller index.html, så /profiles raderas vid varje fs-push. Profilerna
# läses därför ut till en temporär katalog före pushen och skickas tillbaka
# efteråt. Namnen är validerade på enheten (^[A-Za-z0-9_-]{1,32}$), så de kan
# läsas ur JSON-listan med sed utan en JSON-parser.
PROFILE_BACKUP_DIR=""
BACKED_UP=0
RESTORED=0

# Körs även när skriptet dör på set -e: en kvarlämnad katalog är den enda
# kopian av profilerna, så den får aldrig raderas tyst.
backup_left_behind() {
  if [ -n "$PROFILE_BACKUP_DIR" ] && [ -d "$PROFILE_BACKUP_DIR" ]; then
    echo "Profilerna ligger kvar i $PROFILE_BACKUP_DIR" >&2
  fi
}
trap backup_left_behind EXIT

# Drops the temporary copy. Called when it is provably redundant: after a
# successful restore, when the device had no profiles, and when the push itself
# was refused (403/409/... = no image written, so the partition is untouched and
# the profiles are still on the device).
discard_profile_backup() {
  if [ -n "$PROFILE_BACKUP_DIR" ] && [ -d "$PROFILE_BACKUP_DIR" ]; then
    rm -rf "$PROFILE_BACKUP_DIR"
  fi
  PROFILE_BACKUP_DIR=""
}

# Gives up on the backup AND removes what was read: nothing was destroyed yet
# (the push is aborted), so a half-filled directory would only be clutter. The
# trap above therefore stays silent on this path - it is for the one case where
# the copy is the only one left.
backup_abort() {
  echo "$1" >&2
  discard_profile_backup
}

# Names, one per line, from the device's JSON array. An empty array is valid
# (a device with no profiles) and yields nothing.
profile_names() {
  printf '%s' "$1" | tr ',[]' '\n\n\n' |
    sed -n 's/^[[:space:]]*"\([A-Za-z0-9_-]\{1,32\}\)"[[:space:]]*$/\1/p'
}

# Reads every profile off the device into PROFILE_BACKUP_DIR. Returns non-zero
# (and says why) if anything cannot be read - the caller must abort the push
# then, because the push is what destroys them.
backup_profiles() {
  PROFILE_BACKUP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/roaster-profiles.XXXXXX") || {
    PROFILE_BACKUP_DIR=""
    echo "FEL: kunde inte skapa en temporär katalog för profilerna." >&2
    return 1
  }

  LIST_CODE=$(curl -s -o "$PROFILE_BACKUP_DIR/list.json" -w '%{http_code}' \
    --max-time 15 "http://$HOST/api/profiles" 2>/dev/null) || LIST_CODE=000
  if [ "$LIST_CODE" != "200" ]; then
    backup_abort "FEL: kunde inte lista profilerna (HTTP $LIST_CODE) - avbryter FÖRE pushen."
    return 1
  fi
  LIST=$(cat "$PROFILE_BACKUP_DIR/list.json")
  rm -f "$PROFILE_BACKUP_DIR/list.json"
  case "$LIST" in
    \[*\]) ;;
    *) backup_abort "FEL: /api/profiles svarade inte med en JSON-lista - avbryter FÖRE pushen."
       return 1 ;;
  esac

  for name in $(profile_names "$LIST"); do
    body="$PROFILE_BACKUP_DIR/$name.json"
    code=$(curl -s -o "$body" -w '%{http_code}' --max-time 15 \
      "http://$HOST/api/profile?name=$name" 2>/dev/null) || code=000
    if [ "$code" != "200" ]; then
      backup_abort "FEL: kunde inte läsa profilen '$name' (HTTP $code) - avbryter FÖRE pushen."
      return 1
    fi
    case "$(cat "$body")" in
      *'"steps"'*) ;;
      *) backup_abort "FEL: profilen '$name' kom tillbaka utan steg - avbryter FÖRE pushen."
         return 1 ;;
    esac
    BACKED_UP=$((BACKED_UP + 1))
  done

  echo "Säkerhetskopierade profiler: $BACKED_UP (katalog: $PROFILE_BACKUP_DIR)"
  return 0
}

# Writes them back with POST /api/profiles, whose body is exactly what
# GET /api/profile returned (name included). Run only once the device answers
# again: a profile POST during the reboot changes nothing.
restore_profiles() {
  if [ -z "$PROFILE_BACKUP_DIR" ] || [ ! -d "$PROFILE_BACKUP_DIR" ]; then
    return 0
  fi
  if [ "$BACKED_UP" = "0" ]; then
    echo "Inga profiler att återskapa (enheten hade inga)."
    discard_profile_backup
    return 0
  fi

  echo "Återskapar $BACKED_UP profil(er) på enheten..."
  FAILED=0
  for f in "$PROFILE_BACKUP_DIR"/*.json; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .json)
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 \
      -X POST --data-binary "@$f" -H "Content-Type: application/json" \
      "http://$HOST/api/profiles" 2>/dev/null) || code=000
    if [ "$code" = "200" ]; then
      RESTORED=$((RESTORED + 1))
    else
      FAILED=1
      echo "VARNING: profilen '$name' kunde inte återskapas (HTTP $code)."
    fi
  done

  echo "Återskapade profiler: $RESTORED av $BACKED_UP."
  if [ "$FAILED" = "1" ]; then
    echo "FEL: inte alla profiler kom tillbaka. JSON-filerna ligger kvar i"
    echo "      $PROFILE_BACKUP_DIR - skicka dem med:"
    echo "      curl -X POST --data-binary @<fil> -H 'Content-Type: application/json' http://$HOST/api/profiles"
    return 1
  fi
  discard_profile_backup
  return 0
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
    if [ "$PROFILE_BACKUP" = "1" ]; then
      # Läskontroll av det som pushen skulle radera - pushen avbryts om det
      # inte går att läsa, så det är värt att veta redan här.
      DRY_LIST=$(curl -s --max-time 10 "http://$HOST/api/profiles" 2>/dev/null || true)
      case "$DRY_LIST" in
        \[*\]) echo "  profiler att säkerhetskopiera: $(profile_names "$DRY_LIST" | wc -l | tr -d ' ')" ;;
        *)    echo "  profiler: kunde INTE läsas - pushen skulle avbrytas" ;;
      esac
    else
      echo "  profilbackup: AVSTÄNGD (--no-profile-backup)"
    fi
  fi
  exit 0
fi

if [ "$MODE" = "fs" ] && [ "$LOCAL_PAGE_SIZE" != "0" ]; then
  echo "Sidan enheten skickar ut FÖRE pushen: $(served_page_size) byte"
fi

# Profilerna lever i samma partition som fs-imagen skriver över: de läses ut
# innan pushen. Misslyckas det avbryts allt här - en push utan kopia är värre
# än ingen push.
if [ "$MODE" = "fs" ] && [ "$PROFILE_BACKUP" = "1" ]; then
  backup_profiles || exit 1
elif [ "$MODE" = "fs" ]; then
  echo "Profilbackup: AVSTÄNGD (--no-profile-backup) - /profiles raderas av pushen."
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
  # Inget startades om, och för 400/403/404/405/409/413/503 skrevs partitionen
  # aldrig: profilerna finns kvar på enheten och kopian ska inte lämnas efter.
  # 500 är undantaget - där kan halva imagen ha landat, så kopian sparas (trapet
  # ovan skriver ut var den ligger).
  if [ "$CODE" != "500" ]; then
    discard_profile_backup
  fi
  exit 1
fi

# Enheten är tillbaka (000- och 200-vägen är de enda som når hit): profilerna
# som lästes ut före pushen skrivs tillbaka nu. Före sidkontrollen med flit -
# profilerna är data, sidstorleken är ett bevis, och datan ska tillbaka även om
# beviset skulle fallera.
if [ "$MODE" = "fs" ] && [ "$PROFILE_BACKUP" = "1" ]; then
  restore_profiles || exit 1
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
  if [ "$PROFILE_BACKUP" = "1" ]; then
    echo "       Profiler: $RESTORED av $BACKED_UP återskapade på enheten."
  fi
else
  echo "KLART: app-imagen är inne. Kontrollera 'fw' och 'build' ovan mot"
  echo "        git rev-parse --short HEAD på den här maskinen."
fi
