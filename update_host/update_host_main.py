# -*- coding: utf-8 -*-
"""Frozen entrypoint for SAO Auto Update Host."""

from __future__ import annotations

import os

import uvicorn

from app import app


def main() -> None:
    host = os.environ.get("UPDATE_HOST_HOST", "0.0.0.0")
    port = int(os.environ.get("UPDATE_HOST_PORT", "9330"))
    log_level = os.environ.get("UPDATE_HOST_LOG_LEVEL", "info")
    uvicorn.run(app, host=host, port=port, log_level=log_level)


if __name__ == "__main__":
    main()
