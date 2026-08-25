# -*- coding: utf-8 -*-
# Frozen entrypoint for SAO Auto Update Host.

from __future__ import annotations

import os
import threading

import uvicorn

from app import app


def main(stop_event=None) -> None:
    host = os.environ.get("UPDATE_HOST_HOST", "0.0.0.0")
    port = int(os.environ.get("UPDATE_HOST_PORT", "9973"))
    log_level = os.environ.get("UPDATE_HOST_LOG_LEVEL", "info")
    ssl_keyfile = os.environ.get("UPDATE_HOST_SSL_KEYFILE", "").strip()
    ssl_certfile = os.environ.get("UPDATE_HOST_SSL_CERTFILE", "").strip()
    require_tls = os.environ.get("UPDATE_HOST_REQUIRE_TLS", "") == "1"
    if bool(ssl_keyfile) != bool(ssl_certfile):
        raise RuntimeError(
            "UPDATE_HOST_SSL_KEYFILE and UPDATE_HOST_SSL_CERTFILE must be set together"
        )
    if require_tls and not (ssl_keyfile and ssl_certfile):
        raise RuntimeError(
            "UPDATE_HOST_REQUIRE_TLS=1 requires UPDATE_HOST_SSL_KEYFILE and "
            "UPDATE_HOST_SSL_CERTFILE"
        )

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
