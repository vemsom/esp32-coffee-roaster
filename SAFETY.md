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
least 10 % - and it judges on the speed the fan has *reached*, not on the one
that was requested. The duty ramps toward a request
(`FAN_SLEW_PCT_PER_SEC`, 4 s for 0 → 100 %) because the 24 V supply cuts out on
the fan's start current, so a request is not airflow yet; using the request would
light the element while the fan is still spinning up. (`/api/status` reports
both: `fanSpeed` is the request, `fanActual` the reached speed.)

These are the last line of defence, not the first. Software cannot see a loose
mains conductor, a missing heatsink or a wrong wiring diagram.

## Heating without a programme

The machine heats in one of two ways, and only one of them follows a programme:

- **A profile run** (`roastActive` true, `programLoaded` true): the setpoint is
  the profile's curve. `/api/status` names it (`profile`), says which step is
  active (`profileStep`, -1 when none) and what the curve asks for right now
  (`targetTemp`, before the RoR trim).
- **A manual run** (`manualActive` true, `programLoaded` false): the setpoint is
  one temperature typed by the operator, and the run carries an explicit
  duration. When that duration runs out the element is set to 0 % and the run
  ends by itself (`updateControl()` in `src/main.cpp`), and the time left is
  reported as `manualRemainingSeconds` so the bound is visible from outside. The
  firmware does not cap the duration - the web UI offers 0.5-120 min - so the
  bound is only as good as the number that was typed in.

Manual mode is therefore **the only way this machine heats without a programme**,
and it is the mode to look at twice from a monitor.

**A run with no programme behind it is reported as a fault.** A profile file that
parses to zero steps still loads (`RoastProfile::loadFromFile()` only fails when
the file cannot be opened), so profile mode can be entered with no curve to
follow. `targetAt()` is NaN for such a profile, so the element is never commanded
from it - but every other field read like a normal roast, which is why the status
API had no answer at all before 0.17.0. In that state `/api/status` reports
`safetyFault: true` with `safetyReason: "no profile loaded"`, next to
`roastActive: true` and `programLoaded: false`.

That combination is unambiguous: a real safety trip aborts the run
(`abortRunForSafety()` sets the mode to idle), so `safetyFault` and `roastActive`
can only be true together in this one state. The fault is **reported, not
latched**: it disappears with the run and it changes no control law.

**For a monitor** (Home Assistant automation, cron, a person reading the API):
`roastActive: true` alone does not say what is heating. Gate on `programLoaded`
(and compare `profile` with the name you expect) for a run on a curve, or on
`manualActive` plus `manualRemainingSeconds` for the bounded manual run. Anything
else that is heating is the fault above.

## BT-only builds (a missing ET channel)

The firmware can be built with the environment probe switched off entirely:
`SAFETY_ET_ENABLED 0` in `include/config.h` stops `src/sensors.cpp` from reading
the ET converter at all and makes `src/safety.cpp` watch the bean probe alone.
It exists for one situation - a broken ET module while the machine still has to
run - and it is a **reduced-protection build, not a normal mode**:

- **Gone:** every ET trip (`et over temp`, `et sensor fault`, `et sensor stuck`)
  and the cold BT/ET cross-check, which needs two channels and cannot be
  evaluated with one. Nothing replaces the cross-check: it is the only check
  that can see a probe which is wrong while still plausible on its own.
- **Kept, unchanged:** the BT plausibility window, the BT jump check, the BT
  stuck-probe detector, the 260 C BT ceiling, the fan interlock and the latched
  emergency stop. The alarm still clears only after 10 healthy samples in a row -
  with the ET channel off, "healthy" means healthy on BT.
- **Visible everywhere:** `/api/status` reports `etDisabled` and `rorSource` (the
  channel the RoR guidance steers on, `bt` in this build), `safetyReason`
  reads `et disabled - bt only` whenever the latch is clear, the web UI shows a
  standing "ET AVSTÄNGD - endast BT" pill, and Home Assistant gets an
  "ET disabled" problem sensor that is ON. A switched-off probe must never be
  readable as a healthy one.

Behind a frozen BT probe the 260 C ceiling is then the only limit left, and RoR
guidance is still able to move the element: since 0.11.0 it steers on `rorBt`
instead of `rorEt` (`ROR_GUIDANCE_ON_BT` follows `SAFETY_ET_ENABLED`), so a
profile step with `rorTarget > 0` does steer in this mode. Its gain, rate limit
and 30 s window were chosen against an ET curve and have **not** been trimmed
against a BT one - the loop runs, it is not tuned for the bean channel. Do not
roast unattended in this mode.
Build again with the flag at 1 as soon as the ET module is replaced.

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
