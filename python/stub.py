# -*- coding: utf-8 -*-
# Minimal entry stub — compiled to native EXE by Nuitka.
#
# All application code lives in runtime/*.pyd (independently updatable).
# This stub only bootstraps sys.path and hands off to main.pyd.
import os
import sys


def _bootstrap():
    if getattr(sys, "frozen", False):
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
    else:
        exe_dir = os.path.dirname(os.path.abspath(__file__))
    rt = os.path.join(exe_dir, "runtime")
    if rt not in sys.path:
        sys.path.insert(0, rt)
    if exe_dir not in sys.path:
        sys.path.insert(0, exe_dir)


if __name__ == "__main__":
    _bootstrap()
    try:
        from main import main
        sys.exit(main() or 0)
    except Exception as exc:
        try:
            log = os.path.join(os.path.dirname(os.path.abspath(sys.executable)), "crash.log")
            with open(log, "w") as f:
                import traceback
                traceback.print_exc(file=f)
        except Exception:
            pass
        sys.exit(1)
