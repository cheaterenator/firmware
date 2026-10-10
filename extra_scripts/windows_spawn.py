#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
#
# SCons on Windows runs every build command through `cmd /C`, and cmd.exe refuses a
# line over 8191 characters ("The command line is too long."). PlatformIO normally
# spills long compiles into a response file, but platform-native's builder calls
# env.Tool("gcc") / env.Tool("g++") after PlatformIO set that up, which resets
# CCCOM/CXXCOM and drops it. A native-windows compile line carries ~60 library include
# paths plus every userPrefs define, so it reaches cmd's limit with a few private
# userPrefs or one more library.
#
# Start the tools with CreateProcess directly instead; its limit is 32767. This has to be
# a pre: script: SPAWN set here is inherited by the library and project environments
# PlatformIO clones from this one, while a post: script runs after those clones exist.
import os
import shutil
import subprocess
import sys

Import("env")

cmd_spawn = env["SPAWN"]


def spawn_without_cmd(sh, escape, cmd, args, spawn_env):
    # args arrive already quoted for a Windows command line, the same string SCons would
    # hand to cmd /C. Resolve the tool on the build's PATH, since CreateProcess searches
    # only this process's own.
    child_env = {str(k): str(v) for k, v in spawn_env.items()}
    tool = shutil.which(cmd, path=child_env.get("PATH", os.environ.get("PATH")))
    if not tool:
        # A cmd.exe builtin, such as the `del` TEMPFILE runs to remove a response file.
        return cmd_spawn(sh, escape, cmd, args, spawn_env)
    line = " ".join(['"%s"' % tool] + list(args[1:]))
    try:
        return subprocess.call(line, env=child_env)
    except OSError as e:
        sys.stderr.write("%s: %s\n" % (cmd, e))
        return 127


if sys.platform == "win32" and env["PIOENV"].startswith("native-windows"):
    env.Replace(SPAWN=spawn_without_cmd)
