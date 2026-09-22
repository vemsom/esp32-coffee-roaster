# ESP32 Coffee Roaster

Open source ombyggnad av en popcornmaskin till en profilstyrd kafferost, byggd runt en ESP32.

## Status
Tidig utveckling. Se `docs/hardware.md` for hardvarubeslut och `docs/notes.md` for lopande anteckningar.

## Hardvara (planerad)
- ESP32 dev board
- MAX6675 x2 (BT/ET-sensorer, K-type termoelement)
- SSR for varmeelement (time-proportioning-styrning)
- Separat isolerad DC-PSU + MOSFET/motordrivare for flaktmotor (original-kretsen ar EJ isolerad fran natspanning - se docs/hardware.md)

## Licens
TBD (oppen kallkod - MIT eller liknande, avgors innan forsta release)
