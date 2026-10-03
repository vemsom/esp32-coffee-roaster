#!/usr/bin/env python3
"""Stämpla git-sha och byggtid i firmwaren.

PlatformIO kör det här före varje bygge (extra_scripts i platformio.ini) och
resultatet hamnar i build_flags som FW_BUILD_SHA/FW_BUILD_TIME. /api/status
rapporterar dem, så en OTA-push kan bevisa sig själv: jämför "build" i svaret
med `git rev-parse --short HEAD` på maskinen som skickade imagen.

Varför inte bara FW_VERSION: den säger vilken release det är, inte vilken
BYGGE av den som körs. Efter en push är den enda ärliga frågan "landade imagen
jag skickade?", och ett versionsnummer som inte ändrades svarar ingenting.

Repot kan sakna git (zip-nedladdning) eller ha ett smutsigt träd - då står det
"unknown" respektive ett "+" efter shan, hellre än att bygget faller eller
ljuger om vad som ligger i imagen.
"""
import datetime
import os
import subprocess

Import("env")  # noqa: F821  (PlatformIO injicerar den)


def _git(*args):
    try:
        out = subprocess.check_output(
            ("git",) + args, cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL
        )
        return out.decode("utf-8", "replace").strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


sha = _git("rev-parse", "--short", "HEAD") or "unknown"
if sha != "unknown" and _git("status", "--porcelain"):
    # Ett smutsigt träd betyder att imagen inte är exakt den committen. Det ska
    # synas i svaret, inte tigas bort.
    sha += "+dirty"

built = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S") + "Z"

env.Append(  # noqa: F821
    BUILD_FLAGS=[
        '-D FW_BUILD_SHA=\\"%s\\"' % sha,
        '-D FW_BUILD_TIME=\\"%s\\"' % built,
    ]
)

print("[fw_build_id] build %s at %s" % (sha, built))
