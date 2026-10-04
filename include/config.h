#pragma once

// ---- WiFi ----
// Uppgifterna ligger i include/secrets.h (okommiterad). Fyll i där,
// eller låt dessa TBD-varden styra om secrets.h saknas.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "TBD"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "TBD"
#endif

// ---- Språk i användartext ----
// 1 = engelskt (default: den som bygger projektet utan att läsa något får
// engelska i Home Assistant och i webb-UI:t), 0 = svenskt. Byt värdet här -
// det är hela valet. Det styr discovery-namnen (include/strings.h) och
// webbsidan: /api/status rapporterar FW_LANG_CODE och sidan byter text efter
// det (tabellen står i data/index.html, en LittleFS-fil kan inte inkludera
// en .h). Fungerar också som kommandoradsflagga -DFW_LANG_EN=0 (därför
// #ifndef). unique_id/object_id påverkas aldrig av språket - testet i
// tools/host-tests/run.sh kör discovery-testet en gång per språk och jämför
// unique_id-mängderna.
#ifndef FW_LANG_EN
#define FW_LANG_EN 1
#endif

// ---- Sensor-pinnar (SPI, MAX6675 x2) ----
// Avsedda för det egna kretskortet.
//
// Bara KLOCKAN delas: SCLK är ingång på båda modulerna, så den kan inte
// kollidera. SO och CS är privata per modul - en delad SO skulle vila på att
// den oselekterade modulens SO går i tre läge, vilket vi inte kunde bekräfta
// i databladet, och på ett kort kostar den egna returlinjen ingenting.
//
// GPIO5 lämnas helt (strapping-pinne) och GPIO2 undviks (strapping + inbyggd
// lysdiod). 3,3 V och GND delas fortfarande av båda modulerna.
#define PIN_MAX6675_CLK     18  // delad SCLK, ingång på båda modulerna
#define PIN_MAX6675_MISO_BT 19  // egen SO, bön-modulen
#define PIN_MAX6675_MISO_ET 21  // egen SO, miljö-modulen
#define PIN_MAX6675_CS_BT    4  // egen CS, bön-modulen (flyttad från GPIO5 -> GPIO13 -> GPIO4; nu på samma kortsida som CLK/SO/SSR/fan; GPIO4 är ledig och varken strapping eller ADC2)
#define PIN_MAX6675_CS_ET   17  // egen CS, miljö-modulen

// ---- Varme-SSR ----
#define PIN_SSR_HEATER    26

// ---- Flakt-MOSFET (PWM) ----
#define PIN_FAN_PWM        27
#define FAN_PWM_FREQ_HZ    20000
#define FAN_PWM_RESOLUTION 8

// ---- Kontrolloop-timing ----
#define SENSOR_READ_INTERVAL_MS   250
#define HEATER_WINDOW_MS          2000
#define SENSOR_FAULT_MAX_JUMP_C   20.0
#define SENSOR_FAULT_MAX_COUNT    5

// ---- Sensor-kalibrering (enkel offset, tillämpas efter plausibilitetskontrollen) ----
// Mätt 2026-10-03 med båda proberna intill varandra i rumstemperatur (~22-23 C),
// inget element igång. Referens = BT (referenstermometern visade 22-23 C).
// BT läste 24,25-24,75 C (median 24,75), ET läste 29,5-32 C (median 30,75).
// Median-skillnad ET-BT = 6,0 C. BT behölls oförändrad; ET justeras ned med 6 C.
//
// Orsaken till ET:s avvikelse är INTE utredd (kandidater: modulens cold-junction,
// klonchip, probe/placering). Offseten är bara giltig kring rumstemperatur tills
// någon mätt den mot en referens vid rostningstemperatur.
//
// Om du ändrar en av dessa, uppdatera tools/host-tests/test_sensors.cpp och
// docs/firmware-notes.md.
#define SENSOR_OFFSET_BT_C   0.0f
#define SENSOR_OFFSET_ET_C  -6.0f

// ---- Rate of Rise (RoR) ----
// Temperaturändringen per minutt, rapporterad som rorBt/rorEt i /api/status
// och som två läsbara sensorer i Home Assistant. Skattas med LINJÄR
// REGRESSION (least squares) över alla prover som ryms i fönstret - varje
// 250 ms-sample, inte endpoint-delta och inte per-sekund-snapshots. Slopen
// gånger 60000 ger C/min. Negativt värde är legitimt (kylning/turning point)
// och klemmes aldrig; kvantisering och brus hamnar i anpassningen i stället
// för i svaret.
//
// 30 s är bestämt vid första testrosten, och alla prover går in i
// skattningen. 15 s testades som alternativ och ligger kvar som
// alternativ om data talar för det (docs/firmware-notes.md).
#define ROR_WINDOW_MS             30000
// Under så här mycket historia publiceras inget alls: en anpassning över
// några sekunder säger mer om 0,25 C-kvantiseringen än om rostningen. Mellan
// det och fönstret skattas det över de prover som finns - en korrekt C/min,
// bara på kortare underlag.
#define ROR_MIN_SPAN_MS           10000
// Cirkulär buffer med ETT PROV PER SAMPLE (SENSOR_READ_INTERVAL_MS). 128
// poster = 32 s, alltid mer än fönstret, så historiken kan aldrig vara kortare
// än vad regressionen frågar efter. ~2 kB RAM.
#define ROR_BUFFER_LEN            128
// Rate-of-rise guidance gains. RoR guidance is OFF unless a profile step has
// rorTarget > 0, so these only matter when explicitly enabled.
// ROR_GUIDANCE_GAIN scales the error (rorTarget - rorEt) into a duty correction.
// ROR_GUIDANCE_MAX_STEP_PCT_PER_S limits how fast that correction can move per
// second, so a cold probe or a startup transient cannot drive the element full
// power in one sample.
#define ROR_GUIDANCE_GAIN                 3.0f
#define ROR_GUIDANCE_MAX_STEP_PCT_PER_S   15.0f
// The element can never be asked for more than 100 % duty. The PID and the RoR
// correction both clamp to this, and applyHeaterDuty() is the single place
// where the fan interlock can override it.
#define HEATER_MAX_DUTY_PCT               100.0f

// ---- WiFi ----
// Anslutningen startas i setup() utan att vänta, och serviceWifi() i loop()
// kickar om den med sa har mellanrum om den inte ar uppe. Kontrollloopen far
// aldrig stanna pa natverket: rostningen ska fungera aven om accesspunkten
// ar borta eller byter kanal.
#define WIFI_RETRY_INTERVAL_MS    15000

// ---- Flaktsparr (interlock) ----
// Elementet far bara tandas nar flakten ar minst sa har procent. Under det
// klipps varmen omedelbart, med eget felmeddelande (korsa inte ihop det med
// sensorlarmet i safety.cpp). Galler bade manuellt och auto-lage.
#define FAN_MIN_FOR_HEATER_PCT    10

// ---- Sakerhetsgranser (hardkodade skydd, oberoende av PID och profil) ----
// Hard grans for bontemperaturen. Nar den nas slas varmet av och ett larm
// last i safety.cpp - larmet kan inte tystas, det sjalper forst nar
// temperaturen ar tillbaka under gransen med SAFETY_CLEAR_MARGIN_C marginal
// och sensorerna rapporterar friska varden igen.
#define SAFETY_MAX_TEMP_C          260.0
// Hard grans for miljotemperaturen (ET), hogre an BT-gransen med avsikt.
#define SAFETY_MAX_ET_TEMP_C       300.0
// Marginal under gransen som maste uppnas innan larmet sjalper.
#define SAFETY_CLEAR_MARGIN_C       10.0
// Antal felfria sampel i rad som kravs innan ett larm sjalper (10 x 250 ms).
#define SAFETY_CLEAR_STREAK         10
// Plausibilitetsfonster for en enskild MAX6675-avlasning. Utanfor detta raknas
// avlasningen som ett sensorfel (en frilagd ingang kan ge 0 C utan NaN).
//
// Nedre gransen ar 2 C, inte -10 C. En frilagd prober las 0 C - eller ratt
// under ratt over - utan att ge NaN, och ratt 0 hamnade innfor det gamla
// -10..400-fonstret. Det ar precis vad en bench-test med nagot inkopplat
// visade: alla temperaturer 0 C, ingen foljdeslag, elementet kordes pa 100 %
// och larmet drog aldrig. Noll grader hor inte hemma i den har byggnaden -
// kammaren kan inte vara kallare an rummet den star i.
#define SENSOR_MIN_VALID_C          2.0
#define SENSOR_MAX_VALID_C        400.0

// Korskontroll av de tva proberna. Bada hor hemma i samma kammare, sa i
// stillastande batte de ligga nara varandra: en spridning over
// SENSOR_MAX_SPREAD_C nagot av avlasningarna ar fel, aven om var och en ar
// plausibel for sig (t.ex. en prober som star kvar i rumstemperatur medan den
// andra far ratt varde). Kontrollen arbetar bara tills nagot provar passerar
// SENSOR_SPREAD_MAX_COLD_C - nar rostningen batjat far ET och BT skilja sig
// pa riktigt och ska inte jamforas.
#define SENSOR_MAX_SPREAD_C        15.0
#define SENSOR_SPREAD_MAX_COLD_C   60.0

// ---- Fastskad sensor (stuck probe) ----
// En MAX6675 som fryser pa ett plausibelt varde ar osynlig for alla andra
// kontroller: ingen NaN, inget hopp, ingen korsskontroll. Den kan dock inte
// fortsatta ge ett *foranderligt* varde, sa detektorn ar enkel: samma
// avlasning, bit for bit, sa lange att elementet ber om effekt.
//
// Gransen ar SENSOR_STUCK_MAX_MS (60 s). Tva valjningar skyddar mot
// falsklarm:
//
//   * Kontrollen ar endast aktiv nar elementet ber om effekt *just nu*.
//     Kylning mot omgivningen ar fallet som annars larmar i onodan: dar
//     stannar avlasningen i minuter pa slutet, eftersom temperaturandringen
//     blir mindre an en LSB (0,25 C) per minutt - men dar ar elementet for
//     lagt sedan. Med "just nu" finns inget fonnster dar en stallande
//     avlasning kan halka in efter att varmen slutat.
//   * Medan elementet ar av nollstalls timern, sa fonstret bara kan
//     tillbringas under faktisk varme. En prober som star stilla i timmar i
//     ett avstangt maskin far dar inte starta 60-s-rakningen i samma
//     oygonblick som start knapps in - den far en hel period av varme pa sig.
//
// Under en rostning ar 60 s identiska avlasningar omojliga for en levande
// prober: vardet ar kvantiserat till 0,25 C och ligger aldrig stilla nar
// kammaren varmas. Steg 2 i kalibreringen (baslinjebrus) ar vad som
// bekrftar siffran mot riktigt hardvara.
//
// Farligaste riktningen - prober frusen i lagt lane medan PID:n ar pa 100 %
// - ar precis den kontrollen ser, och ET-gransen (SAFETY_MAX_ET_TEMP_C)
// backar upp om ET ar frisk. Om elementet ar av finns ingen overhettning att
// skydda mot, och det ar dar den haller inne.
#define SENSOR_STUCK_MAX_MS       60000

// ---- MQTT (Home Assistant MQTT-discovery) ----
// Anslutningsuppgifterna laggs i include/secrets.h (okommiterad) sa att de
// aldrig hamnar i repot. Sa lange MQTT_HOST ar "TBD" ar MQTT avstangt.
#ifndef MQTT_HOST
#define MQTT_HOST        "TBD"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT        1883
#endif
#ifndef MQTT_USER
#define MQTT_USER        ""
#endif
// MQTT_PASS is accepted as an alias: tooling that fills in secrets.h often
// writes that name instead of MQTT_PASSWORD.
#ifndef MQTT_PASSWORD
#ifdef MQTT_PASS
#define MQTT_PASSWORD    MQTT_PASS
#else
#define MQTT_PASSWORD    ""
#endif
#endif
#define MQTT_BASE_TOPIC             "coffee_roaster"
#define MQTT_DISCOVERY_PREFIX       "homeassistant"
#define MQTT_DEVICE_ID              "coffee_roaster"
#define MQTT_CLIENT_ID              "coffee_roaster_esp32"
#define MQTT_PUBLISH_INTERVAL_MS    2000
#define MQTT_RECONNECT_INTERVAL_MS  5000
// Must stay below the PubSubClient buffer (1024) with room for topic + packet
// header, otherwise the client refuses the publish. Payloads are serialized
// into a fixed buffer instead of a heap String, and an oversized payload is
// reported as an error rather than dropped silently.
#define MQTT_MAX_PAYLOAD_BYTES      900
#define MQTT_CLIENT_BUFFER_BYTES    1024

// ---- OTA (natverksuppdatering, sa att nasta gang inte kraver USB) ----
// ArduinoOTA, samma regel som MQTT: uppdragen ligger i secrets.h och utan
// losenord startas tjansten inte alls. Bilden skrivs till OTA-sloten (app1)
// och enheten startar om nar overforingen ar klar.
//
// Tjansten startar i serviceWifi() sa fort lan ar uppe och handteras i loop().
// Tvao regler gor det sakert:
//
//   * Inget hanteras medan en rostning, ett manuellt lage eller kyldroppet ar
//     igang - handle() anropas helt enkelt inte, sa espota timeoutar i stallet
//     for att avbryta en halv rostning.
//   * ArduinoOTA.onStart() lasar elementet AV innan forsta byten landar. Ingen
//     kontrollcykel kors under overforingen, och ett tidsstalld element som
//     kvarstar i sitt sistalage skulle halla elementet PA under hela
//     uppdateringen. Lasen saknas bara via en explicit nollstallning - och den
//     haller aldrig, eftersom enheten startar om.
#ifndef OTA_PASSWORD
#ifdef OTA_PASS
#define OTA_PASSWORD      OTA_PASS
#else
#define OTA_PASSWORD      ""
#endif
#endif
#define OTA_HOSTNAME       "coffee-roaster"
#define OTA_PORT           3232

// ---- Firmware-version (rapporteras till Home Assistant) ----
// 0.4.0 = de fyra godkanda andringarna. 0.5.0 = OTA tillkommer, sa att de tva
// byggena gar att skillja aven i HA (sw_version ar annars identiskt).
// 0.6.0 = rate of rise (rorBt/rorEt) - nya sensorer i HA, sa ska byggena
// garna att skilja pa igen nar enheten flashas om.
// 0.7.0 = optional RoR guidance per profile step (rorTarget); off by default.
#define FW_VERSION "0.7.0"

// ---- Build identity (FW_BUILD_SHA / FW_BUILD_TIME) ----
// FW_VERSION says which release this is; it cannot say WHICH build of it is
// running. After an OTA push the only honest question is "did the image I just
// sent actually land?", and a version number that did not change answers
// nothing. tools/fw_build_id.py stamps the git sha and the build time into
// build_flags at build time, so /api/status can answer it in one request.
//
// Host tests and any build that skips the stamp still compile: the values fall
// back to "unknown" rather than breaking the build.
#ifndef FW_BUILD_SHA
#define FW_BUILD_SHA "unknown"
#endif
#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

// ---- PID-standardvarden ----
#define PID_KP  4.0
#define PID_KI  0.05
#define PID_KD  2.0

// ---- Profil-lagring ----
#define PROFILES_DIR "/profiles"
