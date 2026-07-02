# -*- coding: utf-8 -*-
"""Razorworks Cyber Menu — subprocess pywebview host.

Standalone script (no package imports needed): loads the dashboard HTML,
exposes a tiny js_api that relays state/actions over a local loopback
socket to the parent process. Spawned by rw_cyber.py, never run directly
by the user.
"""

from __future__ import annotations

import argparse
import json
import os
import threading
from multiprocessing.connection import Client

_HERE = os.path.dirname(os.path.abspath(__file__))


def _html_url() -> str:
    html_path = os.path.join(_HERE, "web", "rw_cyber_dashboard.html")
    return "file:///" + html_path.replace(os.sep, "/")


class RwCyberAPI:
    """Exposed to the dashboard as ``window.pywebview.api``."""

    def __init__(self, port: int, authkey: bytes) -> None:
        self._port = port
        self._authkey = authkey
        self._conn = None
        self._window = None

    def bind_window(self, window) -> None:
        self._window = window

    def connect(self) -> bool:
        try:
            self._conn = Client(("127.0.0.1", self._port), authkey=self._authkey)
            return True
        except Exception:
            return False

    # Called from JS: window.pywebview.api.send_action({...})
    def send_action(self, action) -> dict:
        if self._conn is None:
            return {"ok": False}
        try:
            payload = dict(action) if isinstance(action, dict) else {}
            payload["type"] = "action"
            self._conn.send(payload)
            return {"ok": True}
        except Exception:
            return {"ok": False}

    def ipc_loop(self) -> None:
        while True:
            try:
                msg = self._conn.recv()
            except (EOFError, OSError):
                break
            except Exception:
                break
            if not isinstance(msg, dict):
                continue
            kind = msg.get("type")
            if kind == "state":
                self._push_state(msg)
            elif kind == "shutdown":
                self._close_window()
                break

    def _push_state(self, msg: dict) -> None:
        window = self._window
        if window is None:
            return
        try:
            payload = json.dumps(msg)
        except Exception:
            return
        try:
            window.evaluate_js(
                f"window.__rwApplyState && window.__rwApplyState({payload})")
        except Exception:
            pass

    def _close_window(self) -> None:
        window = self._window
        if window is None:
            return
        try:
            window.destroy()
        except Exception:
            pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--authkey", type=str, required=True)
    parser.add_argument("--title", type=str, required=True)
    parser.add_argument("--w", type=int, default=900)
    parser.add_argument("--h", type=int, default=640)
    args = parser.parse_args()

    import webview

    authkey = bytes.fromhex(args.authkey)
    api = RwCyberAPI(args.port, authkey)
    if not api.connect():
        return

    window = webview.create_window(
        args.title,
        url=_html_url(),
        width=args.w,
        height=args.h,
        x=-10000,
        y=-10000,
        frameless=True,
        easy_drag=False,
        js_api=api,
    )
    api.bind_window(window)

    threading.Thread(target=api.ipc_loop, daemon=True).start()
    webview.start(debug=False)


if __name__ == "__main__":
    main()
