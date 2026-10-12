"""PlatformIO pre-script: stamp build time and git commit into the firmware.

Defines EVG_BUILD_TIME (Unix seconds, UTC), EVG_BUILD_GIT (first 8 hex digits
of HEAD as a 32-bit number) and EVG_BUILD_DIRTY (1 if Firmware/ has
uncommitted changes to tracked files). They are served in DALI memory bank 2
and stored in the "EVGBUILD" record of firmware.bin, so EVG-Updater can check
after an update which build the device actually runs (see dali_bank2.c).

The defines are applied to dali_bank2.c only, so a fresh timestamp rebuilds
that one file instead of the whole project.
"""
import subprocess
import time

Import("env")


def git(*args):
    try:
        return subprocess.check_output(
            ["git", *args], cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


head = git("rev-parse", "--short=8", "HEAD") or "00000000"
dirty = 1 if git("status", "--porcelain", "--untracked-files=no", "--", ".") else 0
build_time = int(time.time())

STAMP = [
    ("EVG_BUILD_TIME", "%dUL" % build_time),
    ("EVG_BUILD_GIT", "0x%sUL" % head),
    ("EVG_BUILD_DIRTY", str(dirty)),
]

print("Build info: %s UTC, git %s%s" % (
    time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(build_time)), head, " (dirty)" if dirty else ""))


def stamp_bank2(env, node):
    return env.Object(node, CPPDEFINES=list(env["CPPDEFINES"]) + STAMP)


env.AddBuildMiddleware(stamp_bank2, "*dali_bank2.c")
