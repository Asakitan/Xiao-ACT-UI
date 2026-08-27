# -*- coding: utf-8 -*-
# Windows SCM host for the Python update service and legacy license compatibility service.
from __future__ import annotations

import argparse
import ctypes
import importlib
import os
import sys
import threading
import traceback
from ctypes import wintypes


SERVICE_WIN32_OWN_PROCESS = 0x10
SERVICE_STOPPED = 1
SERVICE_START_PENDING = 2
SERVICE_STOP_PENDING = 3
SERVICE_RUNNING = 4
SERVICE_ACCEPT_STOP = 1
SERVICE_ACCEPT_SHUTDOWN = 4
SERVICE_ACCEPT_PRESHUTDOWN = 0x100
SERVICE_CONTROL_STOP = 1
SERVICE_CONTROL_INTERROGATE = 4
SERVICE_CONTROL_SHUTDOWN = 5
SERVICE_CONTROL_PRESHUTDOWN = 0xF
NO_ERROR = 0
ERROR_CALL_NOT_IMPLEMENTED = 120
ERROR_FAILED_SERVICE_CONTROLLER_CONNECT = 1063
ERROR_SERVICE_SPECIFIC_ERROR = 1066


class SERVICE_STATUS(ctypes.Structure):
    _fields_ = [
        ("dwServiceType", wintypes.DWORD),
        ("dwCurrentState", wintypes.DWORD),
        ("dwControlsAccepted", wintypes.DWORD),
        ("dwWin32ExitCode", wintypes.DWORD),
        ("dwServiceSpecificExitCode", wintypes.DWORD),
        ("dwCheckPoint", wintypes.DWORD),
        ("dwWaitHint", wintypes.DWORD),
    ]


SERVICE_MAIN_FUNCTION = ctypes.WINFUNCTYPE(
    None, wintypes.DWORD, ctypes.POINTER(wintypes.LPWSTR)
)
SERVICE_HANDLER_FUNCTION = ctypes.WINFUNCTYPE(
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.LPVOID,
    wintypes.LPVOID,
)


class SERVICE_TABLE_ENTRY(ctypes.Structure):
    _fields_ = [
        ("lpServiceName", wintypes.LPWSTR),
        ("lpServiceProc", SERVICE_MAIN_FUNCTION),
    ]


_advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
_advapi32.RegisterServiceCtrlHandlerExW.argtypes = [
    wintypes.LPCWSTR,
    SERVICE_HANDLER_FUNCTION,
    wintypes.LPVOID,
]
_advapi32.RegisterServiceCtrlHandlerExW.restype = ctypes.c_void_p
_advapi32.SetServiceStatus.argtypes = [ctypes.c_void_p, ctypes.POINTER(SERVICE_STATUS)]
_advapi32.SetServiceStatus.restype = wintypes.BOOL
_advapi32.StartServiceCtrlDispatcherW.argtypes = [ctypes.POINTER(SERVICE_TABLE_ENTRY)]
_advapi32.StartServiceCtrlDispatcherW.restype = wintypes.BOOL


_service_config = None
_service_status_handle = None
_service_stop_event = None
_service_status = SERVICE_STATUS()
_service_status_lock = threading.Lock()
_service_handler_callback = None
_service_main_callback = None


def _set_service_status(state: int, exit_code: int = 0, wait_hint: int = 0) -> None:
    global _service_status
    with _service_status_lock:
        accepted = 0
        if state == SERVICE_RUNNING:
            accepted = (
                SERVICE_ACCEPT_STOP
                | SERVICE_ACCEPT_SHUTDOWN
                | SERVICE_ACCEPT_PRESHUTDOWN
            )
        service_exit_code = max(0, int(exit_code))
        _service_status = SERVICE_STATUS(
            SERVICE_WIN32_OWN_PROCESS,
            state,
            accepted,
            ERROR_SERVICE_SPECIFIC_ERROR if service_exit_code else NO_ERROR,
            service_exit_code,
            1 if state in (SERVICE_START_PENDING, SERVICE_STOP_PENDING) else 0,
            max(0, int(wait_hint)),
        )
        handle = _service_status_handle
        status = _service_status
    if handle and not _advapi32.SetServiceStatus(handle, ctypes.byref(status)):
        print(
            f"[server_service] SetServiceStatus failed: {ctypes.get_last_error()}",
            file=sys.stderr,
            flush=True,
        )


def _service_control_handler(control: int, event_type: int, event_data, context) -> int:
    if control in (
        SERVICE_CONTROL_STOP,
        SERVICE_CONTROL_SHUTDOWN,
        SERVICE_CONTROL_PRESHUTDOWN,
    ):
        if _service_stop_event is not None:
            _service_stop_event.set()
        _set_service_status(SERVICE_STOP_PENDING, wait_hint=30000)
        return NO_ERROR
    if control == SERVICE_CONTROL_INTERROGATE:
        with _service_status_lock:
            handle = _service_status_handle
            status = _service_status
        if handle:
            _advapi32.SetServiceStatus(handle, ctypes.byref(status))
        return NO_ERROR
    return ERROR_CALL_NOT_IMPLEMENTED


def _exception_exit_code(exc: BaseException) -> int:
    if isinstance(exc, SystemExit):
        value = exc.code
        if value is None:
            return 0
        return value if isinstance(value, int) else 1
    return 1


def _configure_runtime(config) -> None:
    role_dir = os.path.join(
        config.root, "license_server" if config.role == "license" else "update_host"
    )
    if not os.path.isdir(role_dir):
        raise FileNotFoundError(f"service role directory does not exist: {role_dir}")
    os.chdir(config.root)
    if role_dir not in sys.path:
        sys.path.insert(0, role_dir)
    if config.root not in sys.path:
        sys.path.insert(1, config.root)
    if config.role == "update":
        default_certfile = os.path.join(config.root, "license_server", "server.crt")
        default_keyfile = os.path.join(config.root, "license_server", "server.key")
        certfile = os.environ.get("UPDATE_HOST_SSL_CERTFILE", "").strip() or default_certfile
        keyfile = os.environ.get("UPDATE_HOST_SSL_KEYFILE", "").strip() or default_keyfile
        certfile = os.path.abspath(os.path.expandvars(os.path.expanduser(certfile)))
        keyfile = os.path.abspath(os.path.expandvars(os.path.expanduser(keyfile)))
        if not os.path.isfile(certfile):
            raise FileNotFoundError(
                f"update service TLS certificate file does not exist: {certfile}"
            )
        if not os.path.isfile(keyfile):
            raise FileNotFoundError(
                f"update service TLS private key file does not exist: {keyfile}"
            )
        from tls_pinning import validate_certificate_file
        validate_certificate_file(certfile)
        os.environ["UPDATE_HOST_SSL_CERTFILE"] = certfile
        os.environ["UPDATE_HOST_SSL_KEYFILE"] = keyfile
        os.environ["UPDATE_HOST_REQUIRE_TLS"] = "1"
    if config.port is not None:
        env_name = "SAO_SERVER_PORT" if config.role == "license" else "UPDATE_HOST_PORT"
        os.environ[env_name] = str(config.port)
    log_dir = os.path.join(config.root, "logs")
    os.makedirs(log_dir, exist_ok=True)
    log_path = os.path.join(log_dir, f"{config.service_name}.log")
    stream = open(log_path, "a", encoding="utf-8", buffering=1)
    sys.stdout = stream
    sys.stderr = stream
    print(
        f"[server_service] role={config.role} root={config.root} service={config.service_name}",
        flush=True,
    )


def _run_role(config, stop_event: threading.Event) -> int:
    module_name = "server_main" if config.role == "license" else "update_host_main"
    module = importlib.import_module(module_name)
    role_main = getattr(module, "main")
    try:
        result = role_main(stop_event=stop_event)
    except SystemExit as exc:
        code = _exception_exit_code(exc)
        if code:
            print(f"[server_service] role exited with code {code}", flush=True)
        return code
    except BaseException:
        traceback.print_exc()
        return 1
    return int(result) if isinstance(result, int) else 0


def _service_main(argc: int, argv) -> None:
    global _service_status_handle, _service_stop_event, _service_handler_callback
    config = _service_config
    if config is None:
        return
    _service_stop_event = threading.Event()
    _service_handler_callback = SERVICE_HANDLER_FUNCTION(_service_control_handler)
    _service_status_handle = _advapi32.RegisterServiceCtrlHandlerExW(
        config.service_name, _service_handler_callback, None
    )
    if not _service_status_handle:
        return
    _set_service_status(SERVICE_START_PENDING, wait_hint=30000)
    exit_code = 0
    try:
        _configure_runtime(config)
        _set_service_status(SERVICE_RUNNING)
        exit_code = _run_role(config, _service_stop_event)
    except BaseException as exc:
        exit_code = _exception_exit_code(exc)
        try:
            traceback.print_exc()
        except BaseException:
            pass
    finally:
        _set_service_status(SERVICE_STOP_PENDING, exit_code, wait_hint=30000)
        _set_service_status(SERVICE_STOPPED, exit_code)


def _dispatch_to_scm(config) -> bool:
    global _service_config, _service_main_callback
    _service_config = config
    _service_main_callback = SERVICE_MAIN_FUNCTION(_service_main)
    name_buffer = ctypes.create_unicode_buffer(config.service_name)
    table = (SERVICE_TABLE_ENTRY * 2)()
    table[0] = SERVICE_TABLE_ENTRY(
        ctypes.cast(name_buffer, wintypes.LPWSTR), _service_main_callback
    )
    table[1] = SERVICE_TABLE_ENTRY(None, SERVICE_MAIN_FUNCTION())
    if _advapi32.StartServiceCtrlDispatcherW(table):
        return True
    error = ctypes.get_last_error()
    if error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT:
        return False
    raise OSError(error, "StartServiceCtrlDispatcherW failed")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="server_service.py")
    subparsers = parser.add_subparsers(dest="command", required=True)
    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--service-name", required=True)
    run_parser.add_argument("--role", choices=("license", "update"), required=True)
    run_parser.add_argument("--root", required=True)
    run_parser.add_argument("--port", type=int)
    return parser


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    root = os.path.abspath(os.path.expandvars(os.path.expanduser(args.root)))
    if not os.path.isdir(root):
        parser.error(f"service root directory does not exist: {root}")
    config = argparse.Namespace(
        service_name=args.service_name,
        role=args.role,
        root=root,
        port=args.port,
    )
    if _dispatch_to_scm(config):
        return 0
    stop_event = threading.Event()
    try:
        _configure_runtime(config)
        return _run_role(config, stop_event)
    except BaseException as exc:
        traceback.print_exc()
        return _exception_exit_code(exc)


if __name__ == "__main__":
    raise SystemExit(main())
