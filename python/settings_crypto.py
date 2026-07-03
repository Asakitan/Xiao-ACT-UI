# -*- coding: utf-8 -*-
"""settings_crypto — settings.json 的加密编解码层 (AES-256-GCM + DPAPI)。

被三份 SettingsManager (config.py / gui_modules/settings_manager.py /
sao_webview.py) 以及两处直接读写 settings.json 的插件模块
(star_resonance_plugin.engines.character_profile /
midi_piano_plugin.engine.mp_player) 共用 —— 全部指向同一份磁盘文件,
编解码逻辑必须集中在这一处, 否则任何一处漏改就会把其它读者手里的密文
当明文解析, 互相踩坏数据。

设计:
  - AES key 每次 :func:`encode_settings` 都用 ``os.urandom`` 重新生成
    (AES-256-GCM, Windows CNG/bcrypt.dll 做加解密原语), 用 Windows DPAPI
    (``CryptProtectData``, 当前用户范围) 把这把 key 封起来一起存盘。
    DPAPI 解封只在同一台机器同一个 Windows 账户下才成功 —— 换机器/换
    账户后旧文件视为不可解, 抛 :class:`SettingsCryptoError`, 调用方按
    既有的"损坏文件"逻辑处理 (备份 + 回退默认值), 不会崩溃。
  - 磁盘上仍是一份 JSON, 只是套了层信封::

        {"_enc": "v1", "key": "<base64 DPAPI-protected AES key>",
         "data": "<base64 nonce(12)+tag(16)+ciphertext>"}

    没有 ``"_enc"`` 标记的旧版明文 settings.json (或任意合法 JSON)
    原样按 JSON 解析返回; 三份 SettingsManager 在 load 成功后都会用
    :func:`is_legacy_plaintext` 检查一次, 命中就立刻强制 save 一次
    (不等用户下次改设置) —— 老文件一读就地原地加密, 不丢已有配置。
  - 纯 Windows-only, 零第三方依赖 (只用 ctypes 直调 bcrypt.dll /
    crypt32.dll), 与项目里 act_platform/protect/crypto.py 的既有风格
    一致但不相互 import (那份挂在插件保护子系统的包初始化链上, 直接
    依赖会把整个 ACT 插件体系当成 settings 层的导入副作用, 犯不上)。
"""

from __future__ import annotations

import base64
import ctypes
import json
import os
import tempfile
from typing import Optional

_ENC_MARKER = "_enc"
_ENC_VERSION = "v1"
_ENTROPY = b"SAO-Auto-settings-v1"
_NONCE_LEN = 12
_TAG_LEN = 16


class SettingsCryptoError(Exception):
    """settings.json 无法解密 (损坏 / 非本机本账户加密 / 密钥不符)。"""


# ══════════════════════════════════════════════════════════
#  AES-256-GCM (Windows CNG / bcrypt.dll) —— 数据加解密
# ══════════════════════════════════════════════════════════
class _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("dwInfoVersion", ctypes.c_ulong),
        ("pbNonce", ctypes.c_void_p),
        ("cbNonce", ctypes.c_ulong),
        ("pbAuthData", ctypes.c_void_p),
        ("cbAuthData", ctypes.c_ulong),
        ("pbTag", ctypes.c_void_p),
        ("cbTag", ctypes.c_ulong),
        ("pbMacContext", ctypes.c_void_p),
        ("cbMacContext", ctypes.c_ulong),
        ("cbAAD", ctypes.c_ulong),
        ("cbData", ctypes.c_ulonglong),
        ("dwFlags", ctypes.c_ulong),
    ]


def _open_aes_gcm_key(bcrypt, key: bytes):
    alg_name = "AES\0".encode("utf-16-le")
    chaining_prop = "ChainingMode\0".encode("utf-16-le")
    chaining_gcm = "ChainingModeGCM\0".encode("utf-16-le")

    h_alg = ctypes.c_void_p()
    status = bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(h_alg), alg_name, None, 0)
    if status != 0:
        raise SettingsCryptoError(f"BCryptOpenAlgorithmProvider failed: 0x{status:08X}")
    status = bcrypt.BCryptSetProperty(h_alg, chaining_prop, chaining_gcm, len(chaining_gcm), 0)
    if status != 0:
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)
        raise SettingsCryptoError(f"BCryptSetProperty failed: 0x{status:08X}")

    h_key = ctypes.c_void_p()
    key_buf = (ctypes.c_ubyte * len(key))(*key)
    status = bcrypt.BCryptGenerateSymmetricKey(h_alg, ctypes.byref(h_key), None, 0, key_buf, len(key), 0)
    if status != 0:
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)
        raise SettingsCryptoError(f"BCryptGenerateSymmetricKey failed: 0x{status:08X}")
    return h_alg, h_key


def _aes_gcm_encrypt(key: bytes, plaintext: bytes) -> bytes:
    """AES-256-GCM 加密, 返回 nonce(12)+tag(16)+ciphertext 拼接的 blob。"""
    bcrypt = ctypes.WinDLL("bcrypt")
    h_alg, h_key = _open_aes_gcm_key(bcrypt, key)
    try:
        nonce = os.urandom(_NONCE_LEN)
        tag_buf = (ctypes.c_ubyte * _TAG_LEN)()
        nonce_buf = (ctypes.c_ubyte * _NONCE_LEN)(*nonce)
        pt_buf = (ctypes.c_ubyte * max(1, len(plaintext)))(*plaintext)
        ct_buf = (ctypes.c_ubyte * max(1, len(plaintext)))()

        info = _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO()
        info.cbSize = ctypes.sizeof(info)
        info.dwInfoVersion = 1
        info.pbNonce = ctypes.addressof(nonce_buf)
        info.cbNonce = _NONCE_LEN
        info.pbTag = ctypes.addressof(tag_buf)
        info.cbTag = _TAG_LEN

        cb_result = ctypes.c_ulong(0)
        status = bcrypt.BCryptEncrypt(
            h_key, pt_buf, len(plaintext), ctypes.byref(info), None, 0,
            ct_buf, len(plaintext), ctypes.byref(cb_result), 0)
        if status != 0:
            raise SettingsCryptoError(f"BCryptEncrypt failed: 0x{status:08X}")
        ciphertext = ctypes.string_at(ctypes.addressof(ct_buf), cb_result.value)
        tag = ctypes.string_at(ctypes.addressof(tag_buf), _TAG_LEN)
        return nonce + tag + ciphertext
    finally:
        bcrypt.BCryptDestroyKey(h_key)
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)


def _aes_gcm_decrypt(key: bytes, blob: bytes) -> Optional[bytes]:
    """解密 :func:`_aes_gcm_encrypt` 产出的 blob；校验失败(密钥错/被篡改)返回 None。"""
    if len(blob) < _NONCE_LEN + _TAG_LEN:
        return None
    nonce = blob[:_NONCE_LEN]
    tag = blob[_NONCE_LEN:_NONCE_LEN + _TAG_LEN]
    ciphertext = blob[_NONCE_LEN + _TAG_LEN:]

    bcrypt = ctypes.WinDLL("bcrypt")
    h_alg, h_key = _open_aes_gcm_key(bcrypt, key)
    try:
        nonce_buf = (ctypes.c_ubyte * _NONCE_LEN)(*nonce)
        tag_buf = (ctypes.c_ubyte * _TAG_LEN)(*tag)
        ct_buf = (ctypes.c_ubyte * max(1, len(ciphertext)))(*ciphertext)
        pt_buf = (ctypes.c_ubyte * max(1, len(ciphertext)))()

        info = _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO()
        info.cbSize = ctypes.sizeof(info)
        info.dwInfoVersion = 1
        info.pbNonce = ctypes.addressof(nonce_buf)
        info.cbNonce = _NONCE_LEN
        info.pbTag = ctypes.addressof(tag_buf)
        info.cbTag = _TAG_LEN

        cb_result = ctypes.c_ulong(0)
        status = bcrypt.BCryptDecrypt(
            h_key, ct_buf, len(ciphertext), ctypes.byref(info), None, 0,
            pt_buf, len(ciphertext), ctypes.byref(cb_result), 0)
        if status != 0:
            # STATUS_AUTH_TAG_MISMATCH 或其它 — 篡改/密钥错，不当异常抛出
            return None
        return ctypes.string_at(ctypes.addressof(pt_buf), cb_result.value)
    finally:
        bcrypt.BCryptDestroyKey(h_key)
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)


# ══════════════════════════════════════════════════════════
#  DPAPI (crypt32.dll) —— AES key 的机密封装 (绑定本机+本账户)
# ══════════════════════════════════════════════════════════
class _DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", ctypes.c_ulong), ("pbData", ctypes.POINTER(ctypes.c_ubyte))]


_CRYPTPROTECT_UI_FORBIDDEN = 0x01


def _make_blob(data: bytes):
    """构造 DATA_BLOB; 返回 (blob, 底层buffer) —— 调用方必须在 DPAPI 调用
    结束前一直持有 buffer 的引用, 否则 blob.pbData 会指向已回收的内存。"""
    buf = (ctypes.c_ubyte * len(data))(*data)
    blob = _DATA_BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte)))
    return blob, buf


def _read_blob(blob: "_DATA_BLOB") -> bytes:
    if not blob.pbData or blob.cbData == 0:
        return b""
    addr = ctypes.cast(blob.pbData, ctypes.c_void_p).value
    return ctypes.string_at(addr, blob.cbData)


def _dpapi_protect(data: bytes) -> bytes:
    crypt32 = ctypes.WinDLL("crypt32")
    kernel32 = ctypes.WinDLL("kernel32")
    in_blob, _keep_in = _make_blob(data)
    entropy_blob, _keep_entropy = _make_blob(_ENTROPY)
    out_blob = _DATA_BLOB()
    ok = crypt32.CryptProtectData(
        ctypes.byref(in_blob), None, ctypes.byref(entropy_blob),
        None, None, _CRYPTPROTECT_UI_FORBIDDEN, ctypes.byref(out_blob))
    if not ok:
        raise SettingsCryptoError(f"CryptProtectData failed: 0x{ctypes.GetLastError():08X}")
    try:
        return _read_blob(out_blob)
    finally:
        if out_blob.pbData:
            kernel32.LocalFree(out_blob.pbData)


def _dpapi_unprotect(data: bytes) -> Optional[bytes]:
    crypt32 = ctypes.WinDLL("crypt32")
    kernel32 = ctypes.WinDLL("kernel32")
    in_blob, _keep_in = _make_blob(data)
    entropy_blob, _keep_entropy = _make_blob(_ENTROPY)
    out_blob = _DATA_BLOB()
    ok = crypt32.CryptUnprotectData(
        ctypes.byref(in_blob), None, ctypes.byref(entropy_blob),
        None, None, _CRYPTPROTECT_UI_FORBIDDEN, ctypes.byref(out_blob))
    if not ok:
        return None
    try:
        return _read_blob(out_blob)
    finally:
        if out_blob.pbData:
            kernel32.LocalFree(out_blob.pbData)


# ══════════════════════════════════════════════════════════
#  公开 API —— dict <-> 加密 bytes
# ══════════════════════════════════════════════════════════
def encode_settings(data: dict) -> bytes:
    """dict -> 加密信封 JSON (utf-8 bytes)。每次调用都换一把新 AES key。"""
    plaintext = json.dumps(data, ensure_ascii=False).encode("utf-8")
    aes_key = os.urandom(32)
    cipher_blob = _aes_gcm_encrypt(aes_key, plaintext)
    protected_key = _dpapi_protect(aes_key)
    envelope = {
        _ENC_MARKER: _ENC_VERSION,
        "key": base64.b64encode(protected_key).decode("ascii"),
        "data": base64.b64encode(cipher_blob).decode("ascii"),
    }
    return json.dumps(envelope, indent=2, ensure_ascii=False).encode("utf-8")


def decode_settings(raw: bytes) -> dict:
    """加密信封 bytes -> dict。

    非信封格式 (没有 ``"_enc"`` 标记, 即旧版明文 settings.json 或任意
    合法 JSON) 原样按 JSON 解析返回, 交给调用方在下次 save 时自动迁移
    成加密格式。信封格式但解不开 (换机器/换账户、被篡改) 抛
    :class:`SettingsCryptoError`, 调用方按现有"损坏文件"逻辑处理。
    """
    obj = json.loads(raw.decode("utf-8"))
    if not isinstance(obj, dict):
        raise SettingsCryptoError("settings root must be an object")
    if obj.get(_ENC_MARKER) != _ENC_VERSION:
        return obj
    try:
        protected_key = base64.b64decode(obj["key"])
        cipher_blob = base64.b64decode(obj["data"])
    except Exception as exc:
        raise SettingsCryptoError(f"malformed encrypted envelope: {exc}") from exc
    aes_key = _dpapi_unprotect(protected_key)
    if aes_key is None:
        raise SettingsCryptoError(
            "DPAPI unwrap failed (different machine/account, or corrupted)")
    plaintext = _aes_gcm_decrypt(aes_key, cipher_blob)
    if plaintext is None:
        raise SettingsCryptoError("AES-GCM auth failed (tampered or wrong key)")
    decoded = json.loads(plaintext.decode("utf-8"))
    if not isinstance(decoded, dict):
        raise SettingsCryptoError("decrypted settings root must be an object")
    return decoded


def is_legacy_plaintext(raw: bytes) -> bool:
    """True when ``raw`` parses as JSON but is *not* our encrypted envelope.

    Callers use this right after a successful :func:`decode_settings` load
    to decide whether to force an immediate re-save — migrating an old
    plaintext (or otherwise unencrypted) settings.json to the encrypted
    format on the very next load instead of waiting for the next
    user-triggered settings change.
    """
    try:
        obj = json.loads(raw.decode("utf-8"))
    except Exception:
        return False
    return isinstance(obj, dict) and obj.get(_ENC_MARKER) != _ENC_VERSION


# ══════════════════════════════════════════════════════════
#  便捷文件 I/O —— 给直接读写 settings.json 的插件模块用
#  (character_profile.py / mp_player.py 等原本各自手搓 open()+json,
#  这里统一成一次原子写, 语义仍是"失败就返回空/False", 不抛异常)
# ══════════════════════════════════════════════════════════
def read_settings_file(path: str, *, migrate: bool = False) -> dict:
    """读整份 settings.json; 任何失败 (不存在/损坏/解不开) 都返回 {}。

    ``migrate=True`` 时, 命中旧版明文会用同一次读到的字节原地改写成
    加密格式再返回 (不重复打开文件); 只读不写的调用点 (比如插件的
    "读取角色缓存") 传这个才会主动帮着迁移, 否则老文件要等下一次有人
    写入时才会顺带被加密。
    """
    try:
        if not os.path.exists(path):
            return {}
        with open(path, "rb") as f:
            raw = f.read()
        data = decode_settings(raw)
        if not isinstance(data, dict):
            return {}
        if migrate and is_legacy_plaintext(raw):
            write_settings_file(path, data)
        return data
    except Exception:
        return {}


def write_settings_file(path: str, data: dict) -> bool:
    """原子写入加密后的 settings.json。成功返回 True。"""
    if not isinstance(data, dict):
        return False
    tmp_path = ""
    try:
        blob = encode_settings(data)
        dir_name = os.path.dirname(path) or os.getcwd()
        os.makedirs(dir_name, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=dir_name, delete=False, suffix=".tmp.json",
        ) as tmp:
            tmp.write(blob)
            tmp.flush()
            os.fsync(tmp.fileno())
            tmp_path = tmp.name
        os.replace(tmp_path, path)
        return True
    except Exception:
        try:
            if tmp_path and os.path.exists(tmp_path):
                os.remove(tmp_path)
        except Exception:
            pass
        return False


__all__ = [
    "SettingsCryptoError",
    "encode_settings",
    "decode_settings",
    "is_legacy_plaintext",
    "read_settings_file",
    "write_settings_file",
]
