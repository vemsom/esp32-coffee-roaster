#!/bin/sh
# Uppdatera kaffrostaren över nätet - ingen USB-kabel. ArduinoOTA-vägen.
#
#   sh tools/ota-upload.sh 192.168.x.x        firmware (app-partitionen)
#   sh tools/ota-upload.sh 192.168.x.x fs     webb-UI:t (data/ -> LittleFS)
#
# ÄR DEN HÄR VÄGEN DEFAULT? Nej, tools/ota-push.sh är den för firmware - den
# ansluter TILL enheten och behöver ingen anslutning tillbaka. Det här skriptet
# finns kvar för filsystemet (fs) och för nät där dial-back faktiskt fungerar.
#
# ETT STORT FEL, och två små, värda att känna till:
#
#   1. FS-ÖVERFÖRINGEN MÅSTE AVMONTERA LITTLEFS. Det var det som gjorde att
#      överföringen dog en bit in ("[ERROR]: Error Uploading"), och felet såg ut
#      som ett nätverk. Se src/main.cpp onStart(): U_SPIFFS -> LittleFS.end()
#      före första byten, U_FLASH lämnar filsystemet ifred.
#   2. Returporten pinnas (-P 32320) för att trafiken ska vara förutsägbar.
#      OBS: en dödad körning kan ligga kvar på porten (espota sätter inget
#      SO_REUSEADDR) och nästa bind blir "[ERROR]: Listen Failed" - byt port då.
#   3. -I <serverns LAN-IP> är HYGIEN, inte orsak: den gör lyssnaren entydig i
#      en LXC med virtuella interface. Mätt: en full överföring går igenom både
#      med och utan -I, så den förklarar inte det gamla "No response from
#      device". Behåll den för enkelhetens skull, men skriv den inte som orsak.
#
# VARFÖR SKRIPTET ANROPAR ESPOTA DIREKT i stället för `pio run`:
# PlatformIO:s `upload_flags` sätts ihop till en enda sträng, och den flerradiga
# form som behövs för separata argument ger varje värde ett INLEDANDE
# BLANKSTEG. espota får då host_ip = " <serverns LAN-IP>" och dör på
# "[ERROR]: Listen Failed" innan överföringen ens börjat. Står allt på en rad i
# stället hamnar "-P 32320 -I ..." inuti --auth-värdet
# ("Authenticating...FAIL"). Båda fällorna kostade tid att hitta, och båda
# försvinner när argumenten sätts som en lista: då finns ingen mellanhand som
# kan lägga till ett blanksteg.
#
# LÖSENORDET står inte här och kommer aldrig att stå här: det läses ur
# include/secrets.h (gitignored, 0600) och skickas som --auth i processens
# argument, aldrig i ett skal som loggas.
#
# Första formen skickar firmware.bin till OTA-slotten och startar om.
# Ändringar i data/ (webb-UI:t) följer INTE med den - kör då fs-formen, som
# skickar filsystemsbilden med --spiffs till sin egen partition. Körs den
# aldrig behövs USB (pio run --target uploadfs) som reserv.
#
# Bygg först:  pio run        (fs-formen bygger bilden själv om den saknas)
set -e
cd "$(dirname "$0")/.."

HOST="$1"
if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-upload.sh <ip-eller-host> [fs]"
  echo "  exempel:   sh tools/ota-upload.sh 192.168.x.x"
  echo "             sh tools/ota-upload.sh 192.168.x.x fs   (bara webb-UI:t)"
  exit 2
fi

FS=0
IMAGE=".pio/build/esp32dev/firmware.bin"
if [ "${2:-}" = "fs" ]; then
  FS=1
  IMAGE=".pio/build/esp32dev/littlefs.bin"
  echo "Mål: filsystemet (webb-UI:t) - firmware rörs inte."
  if [ ! -f "$IMAGE" ]; then
    echo "Bygger filsystemsbilden (pio run -t buildfs)..."
    pio run -t buildfs >/dev/null
  fi
else
  echo "Mål: firmware (app-partitionen) - webb-UI:t rörs inte."
  echo "OBS: tools/ota-push.sh är den normala vägen för firmware (ingen dial-back)."
fi

if [ ! -f "$IMAGE" ]; then
  echo "FEL: $IMAGE finns inte - kör 'pio run' först."
  exit 1
fi

PW=$(sed -n 's/^#define[[:space:]]*OTA_PASSWORD[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$PW" ]; then
  echo "FEL: OTA_PASSWORD saknas i include/secrets.h."
  echo "      OTA är avstängt i firmwaren - sätt ett lösenord där först."
  exit 1
fi

ESPOTA=$(find "$HOME/.platformio/packages/framework-arduinoespressif32" \
  -name espota.py -print -quit)
if [ -z "$ESPOTA" ]; then
  echo "FEL: hittar inte espota.py i PlatformIO-paketet."
  exit 1
fi
# Samma Python som PlatformIO använder, så espota får sitt pyserial med.
PY=$(find "$HOME/.platformio/penv/bin" -name 'python3*' -type f -print -quit 2>/dev/null || true)
[ -n "$PY" ] || PY=python3

# Lyssnaren binds till serverns LAN-adress, som läses ur `secrets.h`
# (OTA_ALLOWED_CLIENT_IP - samma adress som firmwaren släpper in). Den står
# uttryckligen och härleds INTE ur default-route: i en LXC med virtuella
# interface är default-route fel svar, och felet yttrar sig som en överföring
# som dör efter "Authenticating...OK".
LISTEN_IP=$(sed -n 's/^#define[[:space:]]*OTA_ALLOWED_CLIENT_IP[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$LISTEN_IP" ]; then
  echo "FEL: OTA_ALLOWED_CLIENT_IP saknas i include/secrets.h."
  echo "      Sätt serverns LAN-adress där (samma värde som firmwaren släpper in)."
  exit 1
fi

set -- --debug --progress --auth "$PW" -P 32320 -I "$LISTEN_IP" -i "$HOST" -f "$IMAGE"
if [ "$FS" = "1" ]; then
  set -- "$@" --spiffs
fi

echo "Skickar $IMAGE till $HOST (espota direkt, -I $LISTEN_IP, -P 32320)"
exec "$PY" "$ESPOTA" "$@"
