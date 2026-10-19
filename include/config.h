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

// Fläktkurva. En 24 V DC-fläkt via MOSFET svarar inte linjärt på PWM-duty:n:
// under en tändtröskel står den still och strax över den stiger varvtalet
// brant. Den uppmätta tröskeln är ~60 % duty (≈14,4 V) på bänken, så en rak
// 0–100-map lägger mer än halva reglerområdet i en död zon. fan_set_speed()
// behåller sitt procent-API oförändrat (0 = av, 1–100 = den logiska hastighet
// som webb-UI:t, profilstegen, MQTT/HA-statusen och fläktspärren talar) och
// mappar 1–100 linjärt på duty-bandet [FAN_DUTY_MIN_PCT, FAN_DUTY_MAX_PCT].
//
// FAN_DUTY_MIN_PCT = lägsta duty fläkten bevisligen snurrar på (den uppmätta
// knät). OVERIFIERAD mot hårdvara: höj tills fläkten startar på lägsta
// inställningen — värdet mäts med den faktiska fläkten. Båda går att sätta vid
// bygget (-DFAN_DUTY_MIN_PCT=65) eftersom de är #ifndef-skyddade.
#ifndef FAN_DUTY_MIN_PCT
#define FAN_DUTY_MIN_PCT   60
#endif
#ifndef FAN_DUTY_MAX_PCT
#define FAN_DUTY_MAX_PCT   100
#endif

// ---- Kontrolloop-timing ----
#define SENSOR_READ_INTERVAL_MS   250
#define HEATER_WINDOW_MS          2000
#define SENSOR_FAULT_MAX_JUMP_C   20.0
#define SENSOR_FAULT_MAX_COUNT    5

// ---- Sensor-kalibrering (enkel offset, tillämpas efter plausibilitetskontrollen) ----
// GALLER NU (mätt 2026-10-04, rumstemperatur/idle): 2-min-medelvärden ur HA
// visade ET ~24,7 C mot BT ~24,2 C, dvs ET låg 0,5 C över BT. ET drogs därför
// ned ytterligare 0,5 C så att de ligger i linje. BT är oförändrad.
//
// Historik: mätt 2026-10-03 med båda proberna intill varandra i rumstemperatur
// (~22-23 C), inget element igång. Referens = BT (referenstermometern visade
// 22-23 C). BT läste 24,25-24,75 C (median 24,75), ET läste 29,5-32 C (median
// 30,75). Median-skillnad ET-BT = 6,0 C; ET drogs då ned 6 C. Den här mätningen
// är historik - använd inte båda siffrorna samtidigt.
//
// Orsaken till ET:s avvikelse är INTE utredd (kandidater: modulens cold-junction,
// klonchip, probe/placering). Offseten är fortfarande bara verifierad kring
// rumstemperatur: ingen mätning mot referens vid rostningstemperatur finns ännu,
// så vid 200-260 C är den en gissning.
//
// Om du ändrar en av dessa, uppdatera tools/host-tests/test_sensors.cpp och
// docs/firmware-notes.md.
#define SENSOR_OFFSET_BT_C   0.0f
#define SENSOR_OFFSET_ET_C  -6.5f

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
// ROR_GUIDANCE_GAIN scales the error (rorTarget - the measured rate: rorEt or
// rorBt, see ROR_GUIDANCE_ON_BT below) into a duty correction.
// ROR_GUIDANCE_MAX_STEP_PCT_PER_S limits how fast that correction can move per
// second, so a cold probe or a startup transient cannot drive the element full
// power in one sample.
#define ROR_GUIDANCE_GAIN                 3.0f
#define ROR_GUIDANCE_MAX_STEP_PCT_PER_S   15.0f
// The element can never be asked for more than 100 % duty. The PID and the RoR
// correction both clamp to this, and applyHeaterDuty() is the single place
// where the fan interlock can override it.
#define HEATER_MAX_DUTY_PCT               100.0f

// ---- Which channel the RoR guidance steers on ----
// The rate of rise is measured on BOTH channels (rorBt, rorEt) and both stay
// published either way; this macro only picks the one the guidance loop
// compares its rorTarget against. 1 = steer on rorBt (the bean mass, the
// denser signal), 0 = steer on rorEt (chamber air).
//
// The default follows the ET channel, and that is the whole point: the two
// build modes must not be combinable into the one combination that fails
// silently. A BT-only build (SAFETY_ET_ENABLED 0) never reads the ET
// converter, so a guidance loop pointed at rorEt would return 0 on every
// cycle - a profile step with rorTarget > 0 would look armed in the web UI and
// never move the element, with no error anywhere to say why. So the source
// follows the channel that is actually measured: ET while it is watched,
// BT when it is not.
//
// The standard build (SAFETY_ET_ENABLED 1, the ET module healthy) therefore
// steers on rorEt exactly as it always has. #ifndef-guarded so a build can
// force the source either way (-DROR_GUIDANCE_ON_BT=0/1) - that is how a host
// test pins one source in a build where both channels exist. The body uses the
// parenthesised expression form on purpose: macros expand where they are USED,
// so SAFETY_ET_ENABLED (defined further down this file) is resolved correctly,
// which an #if here would not be. Steering on ET in a build that does not read
// ET is refused at compile time - see the check under SAFETY_ET_ENABLED.
#ifndef ROR_GUIDANCE_ON_BT
#define ROR_GUIDANCE_ON_BT  (!SAFETY_ET_ENABLED)   // BT-only-build styr på BT
#endif

// The same choice as the string /api/status and the MQTT status payload carry,
// so the machine can always say which channel it is steering on. Reported
// because neither rorBt nor rorEt says which of them the loop is using.
#define ROR_GUIDANCE_SOURCE_NAME (ROR_GUIDANCE_ON_BT ? "bt" : "et")

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

// ---- BT-only build mode: which channels the safety latch watches ----
// 1 (the default) = both probes, exactly as described above and unchanged.
// 0 = BT-only: the ET channel is not used at all. The ET converter is not read
// (src/sensors.cpp reports NaN for it, the same "no reading" a dead module
// gives), and the safety layer watches BT alone (src/safety.cpp).
//
// What is LOST with 0 - precisely, so this is never mistaken for a free switch:
//   * every ET trip: SAFETY_SENSOR_ET, SAFETY_STUCK_ET and SAFETY_OVER_TEMP_ET
//     can no longer latch, so SAFETY_MAX_ET_TEMP_C protects nothing
//   * the cold BT/ET cross-check above (SENSOR_MAX_SPREAD_C /
//     SENSOR_SPREAD_MAX_COLD_C): it needs two channels and cannot be evaluated
//     with one, and it is the only check that sees a probe that is wrong but
//     individually plausible
// RoR guidance is NOT on that list any more (2026-10-17): with the ET channel
// off it steers on rorBt instead of rorEt (ROR_GUIDANCE_ON_BT above), so a
// profile step with rorTarget > 0 still moves the element. What that costs is
// documented in docs/firmware-notes.md: the gains and the 30 s window were
// chosen against an ET curve and have to be trimmed against a BT one.
// What REMAINS, untouched: the BT plausibility window
// (SENSOR_MIN_VALID_C..SENSOR_MAX_VALID_C), the BT jump check, the BT
// stuck-probe detector (SENSOR_STUCK_MAX_MS), the BT ceiling
// (SAFETY_MAX_TEMP_C 260 C), the fan interlock (FAN_MIN_FOR_HEATER_PCT) and
// heater_emergency_off() semantics. The latch still clears only after
// SAFETY_CLEAR_STREAK samples that are healthy and below the margin - with 0,
// "healthy" means healthy on BT, because there is no second channel to be
// healthy or not.
//
// The mode is made visible wherever the machine is observed: /api/status
// reports etDisabled (and rorSource, which follows this flag through
// ROR_GUIDANCE_ON_BT), the web UI shows a standing warning pill, Home
// Assistant gets an ET-disabled problem sensor, and safetyReason reads
// "et disabled - bt only" while the latch is clear instead of "none".
//
// It exists for one situation - a broken ET module while the machine still has
// to run - and is a reduced-protection build by definition. Build it that way
// on purpose, and build again with 1 as soon as the module is replaced:
//   PLATFORMIO_BUILD_FLAGS='-D SAFETY_ET_ENABLED=0' pio run -e esp32-ota
#ifndef SAFETY_ET_ENABLED
#define SAFETY_ET_ENABLED  1
#endif

// The two build modes and the RoR guidance source must not be combinable into
// the one combination that fails silently: steering on rorEt in a build that
// does not read the ET channel leaves updateRorGuidance() returning 0 forever,
// with no error and no visible difference from a step that has no rorTarget.
// The default above cannot produce it (ROR_GUIDANCE_ON_BT follows the flag);
// a forced -DROR_GUIDANCE_ON_BT=0 can, so it is refused here, at build time,
// with the reason in the message. Compiles only with 1 to be checked after the
// flag is known, hence the operator form.
#if !ROR_GUIDANCE_ON_BT && !SAFETY_ET_ENABLED
#error "ROR_GUIDANCE_ON_BT=0 steers the RoR guidance on rorEt, but this build does not read the ET channel (SAFETY_ET_ENABLED 0) - the guidance would be inert. Drop -DROR_GUIDANCE_ON_BT or build with SAFETY_ET_ENABLED=1."
#endif

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
//
// In a BT-only build (SAFETY_ET_ENABLED 0) that ET backup goes away with the
// rest of the ET side: behind a frozen BT probe the BT ceiling
// (SAFETY_MAX_TEMP_C) is then the only limit left, which is one of the reasons
// that mode is a stopgap and not a destination.
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
// 0.8.0 = the fan output now maps the requested 1-100 % onto the duty band the
// fan actually responds in (FAN_DUTY_MIN_PCT..FAN_DUTY_MAX_PCT, default
// 60..100). Same percent API and the same reported/HA values - only the duty
// written to the LEDC channel changed - but that is a change in the fan
// output, so the build has to be distinguishable in Home Assistant.
// 0.9.0 = the explicit BT-only build mode (SAFETY_ET_ENABLED 0): the ET
// channel is not read and its trips are switched off, /api/status reports
// etDisabled, the UI shows a standing warning pill. The version is bumped even
// though the default build (flag 1) is unchanged, because the image on the
// machine is a BT-only one and HA must be able to tell the two apart.
// 0.10.0 = POST /api/update/fs: the filesystem image (the web UI) can now be
// pushed over the network like the app image, instead of only by ArduinoOTA
// (which needs the device to dial back to the uploader - it cannot here). New
// endpoint, so a new release: an app image that answers /api/update/fs and one
// that answers 404 there must be distinguishable after a push.
// 0.11.0 = the RoR guidance can steer on BT: with the ET channel switched off
// (SAFETY_ET_ENABLED 0) a step with rorTarget > 0 now steers on rorBt instead
// of staying inert, and /api/status + the MQTT status payload report which
// channel is in use (rorSource). A behaviour change in the control loop, so a
// new version: a device running 0.10.0 and one running 0.11.0 make different
// decisions on the same profile.
// 0.11.1 = the PID integral is clamped in output units (it was clamped in C*s
// against the 0-100 % output range, which capped the I term at 5 % and left the
// loop P-only ~20 C under its setpoint), KD is 0 (it differentiated a 250 ms
// signal and switched duty between 0 and 100), the control input is filtered
// (the raw sample still reaches the safety layer), and the fan duty slews at
// FAN_SLEW_PCT_PER_SEC instead of jumping to the target (the 24 V supply cut
// out on the fan's start current). New tuning and a new fan output, so a new
// release: this is the first image whose control loop behaves differently on
// the same profile and the same setpoint.
// 0.12.0 = cooling at the end of a run. A profile can carry coolFan/coolMinutes
// and the run ends by itself when its last step has run out, handing over to
// the fan-only cool-down in the same control cycle; POST /api/roast/stop-and-cool
// stops a roast and starts that cooling in ONE request (so the fan never passes
// through 0 % between two); the profile editor exposes the two fields. Until
// now a profile never ended on its own - it held its last setpoint until someone
// pressed Stop - so a profile with the fields set makes a different decision on
// the same file, and the new endpoint does not exist in 0.11.1. New release.
#define FW_VERSION "0.12.0"

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
// STARTING POINT, NOT A TUNED SET (2026-10-17). These numbers have never been
// trimmed against a roast; they are where the first one starts from, and the
// roast is what sets the real values (see docs/firmware-notes.md).
//
// The set they replace (KP 4.0 / KI 0.05 / KD 2.0) was never tested against
// hardware either - it was written before the machine existed - and the first
// measured run with it showed what it really did: BT parked ~20 C under the
// setpoint with duty at 82-86 %, i.e. exactly the steady-state offset of a
// P-only loop. The integral could not do anything about it (its clamp was in
// the wrong unit - see include/pid.h), and KD 2.0 turned 250 ms sampling noise
// into a 0/100 switch. So:
//   * KP 2.0 (from 4.0): the offset the I term now removes does not have to be
//     paid for with a 4 %-duty-per-degree response.
//   * KI 0.15 (from 0.05): enough to take out a 20 C offset within a minute or
//     so of roast time without setting up an oscillation.
//   * KD 0.0 (from 2.0): removed, not trimmed down. A derivative on a signal
//     that jitters +-10 C between samples is a switch; the input filter
//     (PID_INPUT_FILTER_TAU_S) does what the D term was supposed to do, without
//     amplifying the quantisation.
// Trim KP up until the ramp tracks its setpoint, then KI, and only add a small
// KD if a real (filtered) overshoot shows up. Keep this list in step with
// docs/firmware-notes.md.
#define PID_KP  2.0
#define PID_KI  0.15
#define PID_KD  0.0

// ---- Filter on the PID's input (NEVER on the safety path) ----
// Time constant of the one-pole filter on the bean temperature as the control
// loop reads it (include/filter.h). 1 s is a compromise: long enough to take
// the 0.25 C quantisation and the sample-to-sample dither out of the signal the
// P and D terms see, short enough that it is not a delay in the loop (~0.2 C at
// a 10 C/min ramp).
//
// The safety layer is deliberately NOT behind it: safety_update() and the
// jump/plausibility checks see the raw sample from sensors_read(), because a
// dropped probe smoothed by a 1 s filter is a plausible ramp, not a fault.
#define PID_INPUT_FILTER_TAU_S  1.0

// ---- Fläktens startström: slew-rate på duty ----
// The 24 V supply cuts out when the fan is switched straight on at 100 %: the
// start current of a stalled DC motor is too much for it. The duty therefore
// ramps toward the requested speed instead of jumping - 25 %/s, i.e. 0 -> 100 %
// in 4 s. A DECREASE is still written at once: spinning down draws no start
// current, and the heat interlock has to see a requested stop immediately.
//
// The percent API is unchanged (0 = off, 1-100 = requested speed): this is the
// rate at which the fan actually gets there. What the interlock judges on is
// the REACHED speed - fan_actual_pct() - not the request, or the element could
// be lit while the fan is still spinning up.
//
// #ifndef-guarded so a build (or a bisect) can change it:
//   -DFAN_SLEW_PCT_PER_SEC=50     faster
//   -DFAN_SLEW_PCT_PER_SEC=0      no ramp at all (the old immediate write)
#ifndef FAN_SLEW_PCT_PER_SEC
// A plain literal (no `f` suffix) on purpose: the host tests select on this in
// #if, and `25.0f` is not a valid preprocessor expression.
#define FAN_SLEW_PCT_PER_SEC  25.0
#endif

// ---- Profil-lagring ----
#define PROFILES_DIR "/profiles"

// ---- Kylning (fläkt-only) ----
// The cool-down is a fan-only timer: no heater, no PID, no profile - the fan
// runs at `speed` for `minutes` and then stops (updateCool(), src/main.cpp).
// It is reachable from three places, and they all have to accept the same
// values: the cooling panel, "Avbryt och kyl" on a running roast, and the
// per-profile values that start it by themselves when the last step runs out.
// So the limits live here, once, instead of a second set in web_server.cpp.
//
// 1 % is the lowest speed the fan API accepts (0 means off, not a speed), and
// 100 % is the top of the percent scale - the duty curve itself is
// FAN_DUTY_MIN_PCT..FAN_DUTY_MAX_PCT. 0.5 min is the shortest cool-down that
// still moves any air through a drum at 200 C; 120 min is the ceiling the
// cooling panel has always had.
#define COOL_SPEED_MIN_PCT    1
#define COOL_SPEED_MAX_PCT    100
#define COOL_MINUTES_MIN      0.5f
#define COOL_MINUTES_MAX      120.0f

// The defaults for "Avbryt och kyl" (100 % / 5 min) are the `value=` of the two
// fields in data/index.html, not constants here: the page is a LittleFS file and
// cannot include this header. They are 100 % because a cool-down is about
// getting the beans down as fast as the fan can do it, and 5 min because that is
// what the cooling panel already defaults to. Both are editable in the UI, and a
// profile carries its own pair instead of these.
