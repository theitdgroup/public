# PlatformIO pre-build script: give every build a unique time stamp (shown as "build" in /api/status)
import time
Import("env")  # noqa: F821
env.Append(CPPDEFINES=[("FW_BUILD", '\\"%s\\"' % time.strftime("%Y-%m-%d %H:%M:%S"))])  # noqa: F821
