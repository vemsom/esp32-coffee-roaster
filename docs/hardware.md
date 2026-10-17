# Hardware Notes
All recommendations is for 230-240Volt. Make sure you adjust the amperage capability of the SSR based on your local 

## Popcorn maker
Popcorn popper machine similar to [this model](https://www.elgiganten.se/product/hem-hushall-tradgard/koksapparater/bakning-dessert/popcornmaskin/day-popcornmaskin-11887-svart/797278). I think there are several brands of this same machine.

## Fan motor
- 24V DC (check the printing on the actual fan)
- The original circuit uses the heating element as a resistive voltage divider plus a 4-diode bridge and 2 chokes to generate low voltage for the motor.
- IMPORTANT: the original circuit is NOT galvanically isolated from mains despite the low measured voltage. The motor must be powered from its own isolated DC PSU in this build, not from the original circuit.
- Fan driver circuit [example](https://www.amazon.se/-/en/dp/B0DGFT4BTR) 

## Heating element
- Three wires, but **one winding with a tap** - not two halves. Measured 2026-10-17:
  A-B = 7 ohm, B-C = 40 ohm, A-C = 47 ohm (7 + 40 = 47, so B sits 7 ohm in from A).
- That tap is the divider output the original circuit used for the motor's low
  voltage. It is unused in this build and must be left disconnected and insulated.
- Mains goes across **A and C**: 47 ohm ~ 1 125 W / 4.9 A at 230 V. **Never across
  A-B** (7 ohm ~ 7.6 kW / 33 A). B-C (40 ohm ~ 1 320 W) is not used: the section
  that carried the current would then run at ~38 % higher watt density than the
  winding is made for. Wiring and the check before applying mains: `docs/wiring.md`.

## Temperature sensors
- 2x MAX6675 + K-type thermocouples [example](https://www.amazon.se/-/en/dp/B0DRFQ59VT)
- No built-in fault reporting on MAX6675 - sanity check in firmware (see docs/notes.md)

## SSR
- Solid state relay (SSR) with heatsink. [example](https://www.amazon.se/-/en/Heschen-SSR-40DA-Entrance-24-480VAC-Heatsink/dp/B0BDDR8RCW/ref=sr_1_6?crid=2KZXIL0WXW47C&dib=eyJ2IjoiMSJ9.v62nfNZ8s3gIldqc1uRGisAk8Stu96jS5ZVjWsoYhsreCyOk75iVI3htkXA-ZjGSwTKmlDEpm4zbpN0lH5AGqGs3i1zKTsfPw6LDiep0TGNHPU-ZJwHEZk8xcGm8jZ7WBT1UHSqnfpbvKTFGtdJrre3Um3w1Dur0Ry63IebdfkoqP9IBG4QvOCiV8_u_GkTVNil5yb80pQbK6tnl3q07h2KfKiegfkbZGB0wgTOBmY9XKPjo9JHqAqjCAZK0HINnobpXQQ-hEOMcgNx0u_Iao7ZMZX2vf_N3UpowBjcKAYA.6Pn1JkIBRf5G85E7vK3aj8cu62GQKDCDc36h2wkwMDg&dib_tag=se&keywords=ssr&qid=1790231458&sprefix=ssr%2Caps%2C159&sr=8-6) 

## PSU
- 24V DC - To drive the fan [example](https://www.amazon.se/-/en/dp/B0BZDM2W2K)
- 5V DC - To drive the ESP32. Most USB or 5V DC should work; The ESP32's power consumption is very low. 
