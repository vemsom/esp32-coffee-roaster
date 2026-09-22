# ESP32 Coffee Roaster

Open source-ombyggnad av en popcornmaskin till en profilstyrd kafferost, byggd runt en ESP32.

## Status
Tidig utveckling. Se `docs/hardware.md` för hårdvarubeslut och `docs/notes.md` för löpande anteckningar.

## Hårdvara (planerad)
- ESP32 dev board
- MAX6675 x2 (BT/ET-sensorer, K-type termoelement)
- SSR för värmeelement (time-proportioning-styrning)
- Separat isolerad DC-PSU + MOSFET/motordrivare för fläktmotor (original-kretsen är EJ isolerad från nätspänning - se docs/hardware.md)

## Licens
Ej bestämd ännu (öppen källkod - MIT eller liknande, avgörs innan första release)
