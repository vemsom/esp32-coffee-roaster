#!/bin/sh
# Uppdatera kaffrostaren över nätet - ingen USB-kabel.
#
#   sh tools/ota-upload.sh 192.168.0.20
#   sh tools/ota-upload.sh coffee-roaster.local      (samma VLAN)
#
# Bygg först:  pio run
#
# Lösenordet står inte här och kommer aldrig att stå här: det läses ur
# include/secrets.h (gitignored) och exporteras bara i den här processen.
# Är det tomt står firmwarens OTA av, och det här skriptet vägrar köra.
set -e
cd "$(dirname "$0")/.."

HOST="$1"
if [ -z "$HOST" ]; then
  echo "Användning: sh tools/ota-upload.sh <ip-eller-host>"
  echo "  exempel:   sh tools/ota-upload.sh 192.168.0.20"
  exit 2
fi

PW=$(sed -n 's/^#define[[:space:]]*OTA_PASSWORD[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$PW" ]; then
  echo "FEL: OTA_PASSWORD saknas i include/secrets.h."
  echo "      OTA är avstängt i firmwaren - sätt ett lösenord där först."
  exit 1
fi

export OTA_PASSWORD="$PW"
exec pio run -e esp32-ota -t upload --upload-port "$HOST"
