# Hardware Notes

## Base machine
Popcorn popper (same family as https://www.youtube.com/watch?v=U9_8eVlJ1SI)

## Fan motor
- 24V DC (confirmed by measurement)
- The original circuit uses the heating element as a resistive voltage divider plus a 4-diode bridge and 2 chokes to generate low voltage for the motor.
- IMPORTANT: the original circuit is NOT galvanically isolated from mains despite the low measured voltage. The motor must be powered from its own isolated DC PSU in this build, not from the original circuit.

## Temperature sensors
- 2x MAX6675 + K-type thermocouples purchased (arriving separately)
- No built-in fault reporting on MAX6675 - build a sanity check in firmware (see docs/notes.md)

## SSR
- Time-proportioning control, not fast PWM (gentler on a zero-cross SSR)
