# Kopplingschema — ESP32-kaffrostaren

Ställning: ESP32 drivs initialt över **USB** (5 V). Allt annat kopplas i den ordning
som står längst ner. Pinnarna är hämtade ur `include/config.h` (FW 0.3.0) och måste
stämmer mot koden före strömsättning — `git diff` om config.h ändrats.

## Pinout

| Funktion            | GPIO | Anmärkning |
|---------------------|------|------------|
| MAX6675 CLK (delad) | 18   | Delad SCK mellan båda modulerna |
| MAX6675 MISO (delad)| 19   | Delad SO mellan båda modulerna |
| MAX6675 CS — BT     | 5    | Bean temp, egen CS på modul 1 |
| MAX6675 CS — ET     | 17   | Environment temp, egen CS på modul 2 |
| SSR värmestyre      | 26   | Time-proportioning, 2 s fönster |
| Fläkt PWM           | 27   | 20 kHz, 8-bit (0–255 = 0–100 %) |

Strapping-pinnar som INTE får belastas vid boot: **0, 2, 12, 15**. GPIO5 är även
strapping-pin (SDIO-timing) — den får inte hållas låg av en modul vid strömning;
CS är floating på MAX6675-modulen, så det ska vara OK, men bekräfta med mätning.

## Driftträd (strömkällor)

```
USB (dator/5 V-laddare) ──> ESP32                     <── initialt, inga andra 5 V in i VIN
24 V PSU (egen, isolerad) ──> + ──> fläkt (24 V DC)
                             └─ fläktens andra sida ──> fläktmodulens lastkontakt ──> PSU −
24 V PSU −  <─────────────────────────────────── modul GND ──> ESP32 GND   (GEMENSAM JORD krävs)
```

- **Gemensam jord** mellan ESP32, fläktmodul och 24 V-PSU:ns minus. Utan den
  hänger PWM-signalen i luften och fläkten snurrar inte.
- Fläktens ursprungliga krets i popcornmaskinen **ska inte användas** — den är inte
  galvaniskt avskild från nätet. Fläkten drivs bara av den egna 24 V-PSU:n.
- Fyll aldrig 5 V i VIN samtidigt som USB är kopplad.

## 1. MAX6675-modul ×2 (temperatur)

```
Modulens   ESP32
VCC    →   3,3 V          (INTE 5 V — annars går MISO i 5 V mot ESP:s pinnar)
GND    →   GND
SCK    →   GPIO18         (delad)
SO     →   GPIO19         (delad)
CS  #1 →   GPIO5          (BT)
CS  #2 →   GPIO17         (ET)
```

- Termokopplingskabel: `+` till T+/märkt klemm, `−` till T−. K-typ.
- Skruva fast skärmad/plastskruv på termokopplingsklemmen; kabeln ska inte böjas
  skarpt eller dras i klemmen.
- Lägg termokopplingsledningarna **väck från 230 V och från PWM-ledningen**
  (skruva, twistad par) — annars får du drifter i mätningen.

## 2. Fläktmodul (MOSFET, PWM)

```
ESP32 GPIO27  →  modul SIG/PWM
ESP32 GND     →  modul GND          (samma jord som ovan)
Modul last A  ←  fläktens −
Modul last B  →  24 V PSU −
```

- Testa med fläkten fri (inget element i närheten): 10 %, 50 %, 100 % — ska vara
  märkbart olika varvtal.
- **Känd risk:** IRF520 är svag vid 3,3 V gate. Blåser inte fläkten rent på 100 %
  PWM eller MOSFETen blir varm → sätt ett NPN-steg (eller byt modul till en
  logik-nivådriven modul).
- Fläkten måste gå på **minst 10 % innan värmekontaktorn får slå på** — krav i
  firmware (ska bevisas i test 5).

## 3. SSR (elementet)

```
ESP32 GPIO26  →  SSR IN +
ESP32 GND     →  SSR IN −           (optisk isolerad ingång)
Nätet fas     →  SSR IN (1)
Elementets ena ledare ← SSR OUT (2)
Elementets andra ledare → nötrläget (NTC-jord/retur enligt elementets montering)
```

- SSR **måste ha kylfläns** och monteras mot metall som inte är plast.
- **Kontrollera att SSR:n triggrar på 3,3 V** (ingång 3–32 VDC, men håll-in-strömmen
  kan ligga för högt vid 3,3 V) → mät pull-in eller lägg in ett transistorsteg.
- **Popparens ursprungliga styrkrets kopplas ur** — elementet ska enbart gå via SSR.
  Ursprungskretsen är inte avskild från nätet.
- All nätarbete: strömlöst, jordat, och helst med en annan person i närheten.
  Små mätningar med multimeter på spänningslösa kretsar först.

## Ordning vid montering

1. **ESP32 via USB, inget annat** → enheten går upp, webb-UI svarar, MQTT ansluter.
2. **Fläktmodulen** (steg 2 ovan) → test 10/50/100 %, kän på MOSFETens temperatur.
3. **MAX6675 ×2** → lägg båda proberna på samma plats: värdena ska ligga inom några
   grader (det är utgångsläget för BT/ET-korschecken). Notera bruset: detta är
   underlaget för `SENSOR_FAULT_MAX_JUMP_C` (mät ≥5 min, element AV).
4. **SSR** → mätning först, sedan nätet, sist — och först när larm- och
   interlock-testerna är gröna (test 5–6 i `docs/firmware-notes.md`).
