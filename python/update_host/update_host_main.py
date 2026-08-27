# -*- coding: utf-8 -*-
# Frozen entrypoint for SAO Auto Update Host.

from __future__ import annotations

import os
import sys
import threading

import uvicorn

if getattr(sys, "frozen", False):
    HERE = os.path.dirname(sys.executable)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))

_PYTHON_ROOT = os.path.dirname(HERE)
if _PYTHON_ROOT not in sys.path:
    sys.path.insert(0, _PYTHON_ROOT)

from tls_pinning import validate_certificate_file
from app import app


def _resolve_tls_file(env_name: str, default_path: str, label: str) -> str:
    configured = os.environ.get(env_name, "").strip()
    path = configured or default_path
    path = os.path.abspath(os.path.expandvars(os.path.expanduser(path)))
    if not os.path.isfile(path):
        source = f"{env_name}={configured!r}" if configured else f"default {path!r}"
        raise RuntimeError(f"TLS {label} file does not exist ({source}): {path}")
    return path


def main(stop_event=None) -> None:
    host = os.environ.get("UPDATE_HOST_HOST", "0.0.0.0")
    port = int(os.environ.get("UPDATE_HOST_PORT", "9973"))
    log_level = os.environ.get("UPDATE_HOST_LOG_LEVEL", "info")
    os.environ["UPDATE_HOST_REQUIRE_TLS"] = "1"
    default_certfile = os.path.join(HERE, os.pardir, "license_server", "server.crt")
    default_keyfile = os.path.join(HERE, os.pardir, "license_server", "server.key")
    ssl_certfile = _resolve_tls_file(
        "UPDATE_HOST_SSL_CERTFILE", default_certfile, "certificate"
    )
    ssl_keyfile = _resolve_tls_file(
        "UPDATE_HOST_SSL_KEYFILE", default_keyfile, "private key"
    )
    validate_certificate_file(ssl_certfile)

    config = uvicorn.Config(
        app,
        host=host,
        port=port,
        log_level=log_level,
        timeout_keep_alive=120,
        h11_max_incomplete_event_size=16 * 1024 * 1024,
        ssl_keyfile=ssl_keyfile or None,
        ssl_certfile=ssl_certfile or None,
    )
    server = uvicorn.Server(config)
    if stop_event is not None:
        def _watch_stop():
            stop_event.wait()
            server.should_exit = True

        threading.Thread(target=_watch_stop, name="update-stop-watcher", daemon=True).start()
    server.run()


if __name__ == "__main__":
    main()