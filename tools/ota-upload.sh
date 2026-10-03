#!/bin/sh
# Uppdatera kaffrostaren över nätet - ingen USB-kabel. ArduinoOTA-vägen.
#
#   sh tools/ota-upload.sh 192.168.x.x        firmware (app-partitionen)
#   sh tools/ota-upload.sh 192.168.x.x fs     webb-UI:t (data/ -> LittleFS)
#
# ÄR DEN HÄR VÄGEN DEFAULT? Nej. Använd tools/ota-push.sh för firmware: den
# ansluter TILL enheten och behöver ingen anslutning tillbaka. Det här skriptet
# finns kvar för filsystemet (fs) och för nät där dial-back faktiskt fungerar.
#
# TVÅ FALLGROPAR, båda lösta i platformio.ini men värda att känna till:
#
#   1. ArduinoOTA kräver att ENHETEN ansluter tillbaka till uppladdaren på TCP.
#      espota måste därför lyssna på en adress enheten kan nå: serverns LAN-IP.
#      Utan `-I 192.168.1.x` gissar espota sin egen adress, lyssnaren hamnar
#      fel och överföringen dör efter "Authenticating...OK" med
#      "No response from device" - utan att brandväggen har något med saken
#      att göra. Flaggan står i platformio.ini (env esp32-ota).
#   2. Returporten måste vara fast (`-P 32320`) för att en smal brandväggsregel
#      ska kunna matcha; espota slumpar den annars mellan 10000 och 60000.
#
# Kör alltså INTE espota för hand utan att ta med båda flaggorna, och lägg dem
# inte i fel ordning i en variabel: PlatformIO:s flaggmekanik kan skicka med ett
# inledande blanksteg som gör att espota tolkar adressen fel
# ("[ERROR]: Listen Failed"). platformio.ini är rätt plats.
#
# Första kommandot skickar firmware.bin till OTA-slotten och startar om.
# Ändringar i data/ (webb-UI:t) följer INTE med den - kör då andra kommandot,
# som använder samma espota-protokoll med --spiffs och skriver filsystemsbilden
# på sin partition. Körs den aldrig behövs USB (pio run --target uploadfs) som
# reserv.
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
  echo "  exempel:   sh tools/ota-upload.sh 192.168.x.x"
  echo "             sh tools/ota-upload.sh 192.168.x.x fs   (bara webb-UI:t)"
  exit 2
fi

TARGET="upload"
if [ "${2:-}" = "fs" ]; then
  TARGET="uploadfsota"
  echo "Mål: filsystemet (webb-UI:t) - firmware rörs inte."
else
  echo "Mål: firmware (app-partitionen) - webb-UI:t rörs inte."
  echo "OBS: tools/ota-push.sh är den normala vägen för firmware (ingen dial-back)."
fi

PW=$(sed -n 's/^#define[[:space:]]*OTA_PASSWORD[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h | head -n 1)
if [ -z "$PW" ]; then
  echo "FEL: OTA_PASSWORD saknas i include/secrets.h."
  echo "      OTA är avstängt i firmwaren - sätt ett lösenord där först."
  exit 1
fi

export OTA_PASSWORD="$PW"
exec pio run -e esp32-ota -t "$TARGET" --upload-port "$HOST"
