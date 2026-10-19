# Roast profiles - file format

A profile is one JSON file per roast, stored on the device under `/profiles/`
and named after the profile (`/profiles/espresso-1.json`). The web UI is the
normal way to write one (Profiles tab -> name + steps + the two cool-down
fields -> *Spara profil*), but the format is plain enough to hand-write, and
that is what this page documents.

Profiles are also exported and imported as a bundle from the same tab: the
export is `{"version":1,"exported":"<ISO time>","profiles":[<profile>, ...]}`
where each entry is the object below (plus its `name`). Import writes every
entry back through `POST /api/profiles`, so anything that is valid here is valid
there.

## The object

```json
{
  "startTemp": 20,
  "coolFan": 100,
  "coolMinutes": 5,
  "steps": [
    { "ramp": 480, "hold": 120, "temp": 190, "fan": 50 },
    { "ramp": 120, "hold": 480, "temp": 215, "fan": 80,
      "rorTarget": 8, "rorStart": 12, "rorEnd": 4 }
  ]
}
```

| Field | Unit | Meaning |
| --- | --- | --- |
| `startTemp` | °C | The temperature the roast starts from at 0 min. Default 20 (room temperature) if the field is absent. |
| `steps` | array | Ordered list of steps, at least one. See below. |
| `coolFan` | % | Fan speed of the cool-down that starts by itself when the last step has run out. 1-100. **0 or absent = no cooling.** |
| `coolMinutes` | minutes | Length of that cool-down. 0.5-120. **0 or absent = no cooling.** |

`points` is accepted as an alternative to `steps` for very old profiles: a list
of `{"t": seconds, "temp": ..., "fan": ...}` waypoints, each converted into one
ramp step from the previous point's time. New profiles always use `steps`.

## One step

| Field | Unit | Meaning |
| --- | --- | --- |
| `ramp` | seconds | Linear heat-up from the previous step's temperature to `temp`. 0 = no ramp. |
| `hold` | seconds | Time held at `temp` after the ramp. |
| `temp` | °C | Target temperature at the end of the ramp (and during the hold). |
| `fan` | % | Fan speed for this step. The fan steps to this value at the start of the step and holds it - it does not ramp. |
| `rorTarget` | °C/min | Optional rate-of-rise guidance for the step. **0 or absent = off.** |
| `rorStart` | °C/min | Target slope at the beginning of the step, when `rorTarget` is set. |
| `rorEnd` | °C/min | Target slope at the end of the step, when `rorTarget` is set. |

The file is written by the firmware as minutes-in-the-UI but **seconds on
disk** (`ramp`/`hold`), because that is what the control loop works in. The
editor converts both ways.

## Timeline

`ramp 8 min + hold 2 min @ 190 °C` means: 8 minutes of heat-up from wherever the
previous step ended (20 °C for the first one) to 190 °C, then 2 minutes at
190 °C. The next step starts from there. The profile's total length is the sum
of every step's `ramp` + `hold`, and that is what the firmware compares the
roast clock against to decide that the profile is over.

The clock excludes paused time: a pause holds the current setpoint and does not
advance the profile, so a paused profile does not reach its end.

## Automatic cooling after the last step

`coolFan` + `coolMinutes` are one instruction: the fan runs at that speed for
that many minutes, with **no heat**, starting the moment the last step runs out.

- **Both set** (e.g. `100` and `5`): the roast ends by itself and the cooling
  starts in the same control cycle. The element is off, the fan goes straight
  from the last step's fan value to `coolFan` (never through 0 %), and the fan
  stops when the cool-down time is up.
- **Either one 0, missing, or the pair half-filled**: no cooling. The profile
  ends the way it always has - the element goes off and the fan stops. Every
  profile written before these fields existed is in this case, so nothing about
  them changed.
- **Stopping the roast by hand** (Stop, or *Avbryt och kyl*) is not the end of
  the profile: the profile's own cool-down does not start then. *Avbryt och kyl*
  starts the cooling you asked it for, with the values in its own two fields.
- A **safety alarm** aborts the run without starting any cooling: the fan keeps
  running so the beans keep getting air while the fault is handled.

Values are clamped to the limits above when the profile is written and read
(0-100 %, 0-120 min), so a hand-written `999` is stored as `100` rather than
reaching the fan as something the fan cannot do.

## API

| Request | What it does |
| --- | --- |
| `GET /api/profiles` | Names of the stored profiles, as a JSON array. |
| `GET /api/profile?name=<name>` | One profile in the format above, plus `name`. `coolFan`/`coolMinutes` are always present (`0` = none). |
| `POST /api/profiles` | Create/overwrite one profile. Body: the object above plus `"name"`. |
| `DELETE /api/profile?name=<name>` | Remove one profile. |
| `POST /api/roast/start` | `{"profile":"<name>"}` - start a roast on that profile. |
| `POST /api/roast/stop` | Stop the roast. No cooling is started. |
| `POST /api/roast/stop-and-cool` | `{"coolSpeed":100,"coolMinutes":5}` - stop the roast **and** start the fan-only cooling in one command, so the fan never stops in between. `coolSpeed` 1-100, `coolMinutes` 0.5-120; a request outside those limits is refused with 400 and the roast is left running. |
| `POST /api/roast/pause` / `POST /api/roast/resume` | Pause/resume. The clock stops while paused; the heat stays on the current setpoint. |
| `POST /api/cool/start` | `{"speed":100,"minutes":5}` - start the fan-only cooling by hand. |
| `POST /api/cool/stop` | Stop the cooling (fan off). |

A profile name has to match `[A-Za-z0-9_-]{1,32}`: it is also the file name, so
anything else is refused with 400 rather than written. (The web UI's name field
is not filtered the same way - *Spara profil* will surface the refusal.)
