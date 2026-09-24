# ESP32 Coffee Roaster

Open source conversion of a popcorn popper into a profile-driven coffee roaster, built around an ESP32.

## Status
Early development. See `docs/hardware.md` for hardware decisions and `docs/notes.md` for ongoing notes.

## Hardware
- ESP32 dev board
- MAX6675 x2 (BT/ET sensors, K-type thermocouples)
- SSR for the heating element (time-proportioning control)
- Separate isolated DC PSU + MOSFET motor driver for the fan (the original popper circuit is NOT isolated from mains - see docs/hardware.md)

## License
Not decided yet (open source - MIT or similar, to be finalized before first release)
