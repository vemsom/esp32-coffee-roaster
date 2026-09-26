#!/bin/sh
# Uppdatera kaffrostaren över nätet - ingen USB-kabel.
#
#   sh tools/ota-upload.sh 192.168.0.20        firmware (app-partitionen)
#   sh tools/ota-upload.sh 192.168.0.20 fs     webb-UI:t (data/ -> LittleFS)
#
# Första kommandot är det vanliga: det skickar firmware.bin till OTA-slotten
# och startar om. Ändringar i data/ (webb-UI:t) följer INTE med den - kör då
# andra kommandot, som använder samma espota-protokoll med --spiffs och
# skriver filsystemsbilden på sin partition. Körs den aldrig behövs USB
# (pio run --target uploadfs) som reserv.
#
# Bygg först:  pio run        (och pio run -t buildfs om UI:t ändrats)
#
# Lösenordet står inte här och kommer aldrig att stå här: det läses ur
# include/secrets.h (gitignored) och exporteras bara i den här processen.
# Är det tomt står firmwarens OTA av, och det här skriptet vägrar köra.
set -e
cd "$(dirname "$0")/.."

HOST="$1"
if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-upload.sh <ip-eller-host> [fs]"
  echo "  exempel:   sh tools/ota-upload.sh 192.168.0.20"
  echo "             sh tools/ota-upload.sh 192.168.0.20 fs   (bara webb-UI:t)"
  exit 2
fi

TARGET="upload"
if [ "${2:-}" = "fs" ]; then
  TARGET="uploadfsota"
  echo "Mål: filsystemet (webb-UI:t) - firmware rörs inte."
else
  echo "Mål: firmware (app-partitionen) - webb-UI:t rörs inte."
fi

PW=$(sed -n 's/^#define[[:space:]]*OTA_PASSWORD[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$PW" ]; then
  echo "FEL: OTA_PASSWORD saknas i include/secrets.h."
  echo "      OTA är avstängt i firmwaren - sätt ett lösenord där först."
  exit 1
fi

export OTA_PASSWORD="$PW"
exec pio run -e esp32-ota -t "$TARGET" --upload-port "$HOST"
