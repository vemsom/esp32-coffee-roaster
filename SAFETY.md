# Safety

This project switches **mains voltage (230-240 V AC)** with a solid-state relay
that the ESP32 controls. If you are not comfortable working around mains, stop
here and buy a roaster instead. Everything below is a backstop, not a permit to
be careless.

## The big ones

- **Isolate the mains before you touch anything.** Never wire, probe or move the
  heating element with the mains connected. Work on a dead circuit, and prefer a
  second person in the room.
- **The stock fan circuit inside the popper is not galvanically isolated from
  mains**, even though it measures a low voltage. Do not reuse it. In this build
  the fan runs from its own isolated 24 V DC supply, and the heater runs only
  through the SSR.
- **Fit a heatsink to the SSR** and mount it against metal, not plastic. Check
  that your SSR actually triggers at 3.3 V.
- **Add a 10-47 kOhm pull-down between SSR IN+ and GND.** Between reset and the
  first instruction of `setup()`, the GPIO is a floating input; the firmware
  drives it low immediately after, but the pull-down is the only thing that
  covers those first milliseconds.
- **Keep the thermocouple leads away from the mains wiring and from the fan PWM
  line** (twist them), or the readings will drift.

## What the firmware does, and what it does not

The heater sits behind a **latched alarm** that no command can silence. It trips
on a hard temperature limit, a sensor fault (NaN, out-of-range or an implausible
jump), two cold probes disagreeing, or a probe stuck on the same reading while
the element is asking for power. While it is latched the relay is held off, a
running roast is aborted and new runs are refused, and the fan keeps running. The
latch is stored in flash and survives a power cycle.

Separately, a **fan interlock** holds the element off unless the fan runs at
least 10 %.

These are the last line of defence, not the first. Software cannot see a loose
mains conductor, a missing heatsink or a wrong wiring diagram.

## Operating

- Never leave a roast running unattended. The alarm assumes someone is there.
- Do not power the ESP32 from 5 V VIN at the same time as USB.
- Do not flash the heater or the fan while the machine is mid-roast; the
  firmware refuses an update while a session runs or the element is asking for
  power, but pull the mains if you are doing anything else with the wiring.

## Read before you build

- `docs/hardware.md` - the parts and the hardware decisions.
- `docs/wiring.md` - the pin-by-pin connections and the order to assemble in.

This is a hobby project with a small safety net and no certification. You build
and operate it at your own risk.
