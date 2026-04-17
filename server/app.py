# -*- coding: utf-8 -*-
"""FastAPI server for SAO auto-key profile repository."""

import base64
import hashlib
import hmac
import json
import os
import secrets
import sqlite3
import sys
import time
from contextlib import asynccontextmanager, contextmanager
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, Header, HTTPException, Query
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

if getattr(sys, 'frozen', False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(BASE_DIR, "data")
DB_PATH = os.path.join(DATA_DIR, "scripts.db")
UPLOAD_SECRET_PATH = os.path.join(DATA_DIR, "upload_secret.txt")
DEFAULT_UPLOAD_TOKEN_TTL_SECONDS = 15 * 60
_CACHED_UPLOAD_SECRET = None


@asynccontextmanager
async def _lifespan(app_: FastAPI):
    _ensure_db()
    yield


app = FastAPI(title="SAO Auto Key Repository", version="2.0.0", lifespan=_lifespan)


class UploadScriptPayload(BaseModel):
    profile_name: str = Field(default="")
    description: str = Field(default="")
    profession_id: int = Field(default=0)
    profession_name: str = Field(default="")
    player_uid: str = Field(default="")
    player_name: str = Field(default="")
    schema_version: int = Field(default=1)
    profile: Dict[str, Any] = Field(default_factory=dict)


class UploadBossRaidPayload(BaseModel):
    profile_name: str = Field(default="")
    description: str = Field(default="")
    boss_total_hp: int = Field(default=0)
    enrage_time_s: int = Field(default=600)
    player_uid: str = Field(default="")
    player_name: str = Field(default="")
    schema_version: int = Field(default=1)
    profile: Dict[str, Any] = Field(default_factory=dict)


class IssueUploadTokenPayload(BaseModel):
    player_uid: str = Field(default="")
    player_name: str = Field(default="")
    profession_id: int = Field(default=0)
    profession_name: str = Field(default="")


def _utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _upload_secret() -> str:
    global _CACHED_UPLOAD_SECRET
    env_secret = os.environ.get("SAO_UPLOAD_SECRET", "").strip()
    if env_secret:
        _CACHED_UPLOAD_SECRET = env_secret
        return env_secret
    if _CACHED_UPLOAD_SECRET:
        return _CACHED_UPLOAD_SECRET
    os.makedirs(DATA_DIR, exist_ok=True)
    try:
        with open(UPLOAD_SECRET_PATH, "r", encoding="utf-8") as f:
            file_secret = f.read().strip()
    except FileNotFoundError:
        file_secret = ""
    if not file_secret:
        file_secret = secrets.token_hex(32)
        with open(UPLOAD_SECRET_PATH, "w", encoding="utf-8") as f:
            f.write(file_secret)
    _CACHED_UPLOAD_SECRET = file_secret
    return file_secret


def _legacy_upload_token() -> str:
    return os.environ.get("SAO_UPLOAD_TOKEN", "").strip()


def _upload_token_ttl_seconds() -> int:
    try:
        return max(60, int(os.environ.get("SAO_UPLOAD_TOKEN_TTL_SECONDS", DEFAULT_UPLOAD_TOKEN_TTL_SECONDS)))
    except Exception:
        return DEFAULT_UPLOAD_TOKEN_TTL_SECONDS


def _b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def _b64url_decode(text: str) -> bytes:
    pad = "=" * (-len(text) % 4)
    return base64.urlsafe_b64decode((text + pad).encode("ascii"))


def _normalize_identity(player_uid: str, player_name: str, profession_id: int, profession_name: str = "") -> Dict[str, Any]:
    return {
        "player_uid": (player_uid or "").strip(),
        "player_name": (player_name or "").strip(),
        "profession_id": int(profession_id or 0),
        "profession_name": (profession_name or "").strip(),
    }


def _ensure_identity_ready(identity: Dict[str, Any]) -> Dict[str, Any]:
    missing: List[str] = []
    if not identity["player_uid"]:
        missing.append("player_uid")
    if not identity["player_name"]:
        missing.append("player_name")
    if int(identity["profession_id"] or 0) <= 0:
        missing.append("profession_id")
    if missing:
        raise HTTPException(status_code=400, detail=f"Missing identity fields: {', '.join(missing)}")
    return identity


def _sign_upload_claims(payload_b64: str, secret: str) -> str:
    return hmac.new(secret.encode("utf-8"), payload_b64.encode("utf-8"), hashlib.sha256).hexdigest()


def _issue_signed_upload_token(identity: Dict[str, Any]) -> Dict[str, Any]:
    secret = _upload_secret()
    if not secret:
        raise RuntimeError("Server upload secret is not configured")
    exp = int(time.time()) + _upload_token_ttl_seconds()
    claims = {
        "uid": identity["player_uid"],
        "name": identity["player_name"],
        "profession_id": int(identity["profession_id"] or 0),
        "exp": exp,
    }
    claims_json = json.dumps(claims, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    payload_b64 = _b64url_encode(claims_json)
    return {
        "token": f"{payload_b64}.{_sign_upload_claims(payload_b64, secret)}",
        "expires_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(exp)),
        "mode": "signed",
        "claims": claims,
    }


def _verify_signed_upload_token(token: str) -> Dict[str, Any]:
    secret = _upload_secret()
    if not secret:
        raise RuntimeError("Server upload secret is not configured")
    try:
        payload_b64, signature = token.split(".", 1)
    except ValueError as exc:
        raise RuntimeError("Invalid upload token format") from exc
    expected_sig = _sign_upload_claims(payload_b64, secret)
    if not hmac.compare_digest(signature, expected_sig):
        raise RuntimeError("Invalid upload token signature")
    try:
        claims = json.loads(_b64url_decode(payload_b64).decode("utf-8"))
    except Exception as exc:
        raise RuntimeError("Invalid upload token payload") from exc
    if not isinstance(claims, dict):
        raise RuntimeError("Invalid upload token payload")
    if int(claims.get("exp") or 0) <= int(time.time()):
        raise RuntimeError("Upload token expired")
    uid = str(claims.get("uid") or "").strip()
    name = str(claims.get("name") or "").strip()
    profession_id = int(claims.get("profession_id") or 0)
    if not uid or not name or profession_id <= 0:
        raise RuntimeError("Upload token identity is incomplete")
    return {
        "player_uid": uid,
        "player_name": name,
        "profession_id": profession_id,
    }


def _ensure_db():
    os.makedirs(DATA_DIR, exist_ok=True)
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS script_profiles (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                profile_name TEXT NOT NULL,
                description TEXT NOT NULL,
                profession_id INTEGER NOT NULL DEFAULT 0,
                profession_name TEXT NOT NULL,
                player_uid TEXT NOT NULL,
                player_name TEXT NOT NULL,
                script_json TEXT NOT NULL,
                schema_version INTEGER NOT NULL DEFAULT 1,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS boss_raid_profiles (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                profile_name TEXT NOT NULL,
                description TEXT NOT NULL DEFAULT '',
                boss_total_hp INTEGER NOT NULL DEFAULT 0,
                enrage_time_s INTEGER NOT NULL DEFAULT 600,
                player_uid TEXT NOT NULL DEFAULT '',
                player_name TEXT NOT NULL DEFAULT '',
                profile_json TEXT NOT NULL DEFAULT '{}',
                schema_version INTEGER NOT NULL DEFAULT 1,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            )
            """
        )
        conn.commit()


@contextmanager
def _db():
    _ensure_db()
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
    finally:
        conn.close()


def _row_to_summary(row: sqlite3.Row) -> Dict[str, Any]:
    return {
        "id": int(row["id"]),
        "profile_name": row["profile_name"],
        "description": row["description"],
        "profession_id": int(row["profession_id"] or 0),
        "profession_name": row["profession_name"],
        "player_uid": row["player_uid"],
        "player_name": row["player_name"],
        "schema_version": int(row["schema_version"] or 1),
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _row_to_detail(row: sqlite3.Row) -> Dict[str, Any]:
    summary = _row_to_summary(row)
    try:
        summary["profile"] = json.loads(row["script_json"] or "{}")
    except Exception:
        summary["profile"] = {}
    return summary


@app.get("/health")
def health():
    return {"ok": True, "db_path": DB_PATH}


@app.get("/api/scripts")
def search_scripts(
    q: str = Query(default=""),
    profile_name: str = Query(default=""),
    player_uid: str = Query(default=""),
    player_name: str = Query(default=""),
    profession_name: str = Query(default=""),
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=20, ge=1, le=100),
):
    where: List[str] = []
    params: List[Any] = []
    if q:
        like = f"%{q.strip()}%"
        where.append("(profile_name LIKE ? OR player_uid LIKE ? OR player_name LIKE ? OR profession_name LIKE ?)")
        params.extend([like, like, like, like])
    if profile_name:
        where.append("profile_name LIKE ?")
        params.append(f"%{profile_name.strip()}%")
    if player_uid:
        where.append("player_uid LIKE ?")
        params.append(f"%{player_uid.strip()}%")
    if player_name:
        where.append("player_name LIKE ?")
        params.append(f"%{player_name.strip()}%")
    if profession_name:
        where.append("profession_name LIKE ?")
        params.append(f"%{profession_name.strip()}%")
    where_sql = f"WHERE {' AND '.join(where)}" if where else ""
    offset = (page - 1) * page_size
    with _db() as conn:
        total = conn.execute(f"SELECT COUNT(*) FROM script_profiles {where_sql}", params).fetchone()[0]
        rows = conn.execute(
            f"""
            SELECT * FROM script_profiles
            {where_sql}
            ORDER BY updated_at DESC, id DESC
            LIMIT ? OFFSET ?
            """,
            params + [page_size, offset],
        ).fetchall()
    return {
        "items": [_row_to_summary(row) for row in rows],
        "page": page,
        "page_size": page_size,
        "total": int(total or 0),
    }


@app.get("/api/scripts/{script_id}")
def get_script(script_id: int):
    with _db() as conn:
        row = conn.execute("SELECT * FROM script_profiles WHERE id = ?", [script_id]).fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Script not found")
    return _row_to_detail(row)


@app.get("/api/scripts/{script_id}/export")
def export_script(script_id: int):
    with _db() as conn:
        row = conn.execute("SELECT * FROM script_profiles WHERE id = ?", [script_id]).fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Script not found")
    detail = _row_to_detail(row)
    payload = {
        "schema_version": int(detail.get("schema_version") or 1),
        "profile": detail.get("profile") or {},
    }
    return JSONResponse(content=payload)


@app.post("/api/upload-token/issue")
def issue_upload_token(payload: IssueUploadTokenPayload):
    identity = _ensure_identity_ready(_normalize_identity(
        payload.player_uid,
        payload.player_name,
        payload.profession_id,
        payload.profession_name,
    ))
    if _upload_secret():
        issued = _issue_signed_upload_token(identity)
        return {
            "token": issued["token"],
            "expires_at": issued["expires_at"],
            "identity": identity,
            "mode": issued["mode"],
        }
    legacy = _legacy_upload_token()
    if legacy:
        return {
            "token": legacy,
            "expires_at": "",
            "identity": identity,
            "mode": "legacy",
        }
    raise HTTPException(status_code=500, detail="Server upload secret/token is not configured")


@app.post("/api/scripts")
def upload_script(payload: UploadScriptPayload, x_sao_upload_token: Optional[str] = Header(default="")):
    provided_token = (x_sao_upload_token or "").strip()
    signed_identity = None
    if _upload_secret():
        try:
            signed_identity = _verify_signed_upload_token(provided_token)
        except RuntimeError:
            signed_identity = None
    if signed_identity is None:
        legacy = _legacy_upload_token()
        if legacy:
            if provided_token != legacy:
                raise HTTPException(status_code=401, detail="Invalid upload token")
        elif _upload_secret():
            raise HTTPException(status_code=401, detail="Invalid upload token")
        else:
            raise HTTPException(status_code=500, detail="Server upload secret/token is not configured")
    if signed_identity is not None:
        payload_identity = _normalize_identity(
            payload.player_uid,
            payload.player_name,
            payload.profession_id,
            payload.profession_name,
        )
        if payload_identity["player_uid"] != signed_identity["player_uid"] or \
           payload_identity["player_name"] != signed_identity["player_name"] or \
           int(payload_identity["profession_id"] or 0) != int(signed_identity["profession_id"] or 0):
            raise HTTPException(status_code=401, detail="Upload token identity does not match payload")
    now = _utc_now_iso()
    with _db() as conn:
        cursor = conn.execute(
            """
            INSERT INTO script_profiles (
                profile_name, description,
                profession_id, profession_name,
                player_uid, player_name,
                script_json, schema_version,
                created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                payload.profile_name.strip(),
                payload.description.strip(),
                int(payload.profession_id or 0),
                payload.profession_name.strip(),
                payload.player_uid.strip(),
                payload.player_name.strip(),
                json.dumps(payload.profile, ensure_ascii=False),
                int(payload.schema_version or 1),
                now,
                now,
            ],
        )
        conn.commit()
        new_id = int(cursor.lastrowid)
        row = conn.execute("SELECT * FROM script_profiles WHERE id = ?", [new_id]).fetchone()
    return _row_to_detail(row)


# ═══════════════════════════════════════════════
#  Boss Raid Profiles API
# ═══════════════════════════════════════════════

def _br_row_to_summary(row: sqlite3.Row) -> Dict[str, Any]:
    return {
        "id": int(row["id"]),
        "profile_name": row["profile_name"],
        "description": row["description"],
        "boss_total_hp": int(row["boss_total_hp"] or 0),
        "enrage_time_s": int(row["enrage_time_s"] or 600),
        "player_uid": row["player_uid"],
        "player_name": row["player_name"],
        "schema_version": int(row["schema_version"] or 1),
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _br_row_to_detail(row: sqlite3.Row) -> Dict[str, Any]:
    summary = _br_row_to_summary(row)
    try:
        summary["profile"] = json.loads(row["profile_json"] or "{}")
    except Exception:
        summary["profile"] = {}
    return summary


@app.get("/api/boss-raids")
def search_boss_raids(
    q: str = Query(default=""),
    profile_name: str = Query(default=""),
    player_uid: str = Query(default=""),
    player_name: str = Query(default=""),
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=20, ge=1, le=100),
):
    where: List[str] = []
    params: List[Any] = []
    if q:
        like = f"%{q.strip()}%"
        where.append("(profile_name LIKE ? OR player_uid LIKE ? OR player_name LIKE ?)")
        params.extend([like, like, like])
    if profile_name:
        where.append("profile_name LIKE ?")
        params.append(f"%{profile_name.strip()}%")
    if player_uid:
        where.append("player_uid LIKE ?")
        params.append(f"%{player_uid.strip()}%")
    if player_name:
        where.append("player_name LIKE ?")
        params.append(f"%{player_name.strip()}%")
    where_sql = f"WHERE {' AND '.join(where)}" if where else ""
    offset = (page - 1) * page_size
    with _db() as conn:
        total = conn.execute(f"SELECT COUNT(*) FROM boss_raid_profiles {where_sql}", params).fetchone()[0]
        rows = conn.execute(
            f"""
            SELECT * FROM boss_raid_profiles
            {where_sql}
            ORDER BY updated_at DESC, id DESC
            LIMIT ? OFFSET ?
            """,
            params + [page_size, offset],
        ).fetchall()
    return {
        "items": [_br_row_to_summary(row) for row in rows],
        "page": page,
        "page_size": page_size,
        "total": int(total or 0),
    }


@app.get("/api/boss-raids/{raid_id}")
def get_boss_raid(raid_id: int):
    with _db() as conn:
        row = conn.execute("SELECT * FROM boss_raid_profiles WHERE id = ?", [raid_id]).fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Boss raid profile not found")
    return _br_row_to_detail(row)


@app.post("/api/boss-raids")
def upload_boss_raid(payload: UploadBossRaidPayload,
                     x_sao_upload_token: Optional[str] = Header(default="")):
    provided_token = (x_sao_upload_token or "").strip()
    signed_identity = None
    if _upload_secret():
        try:
            signed_identity = _verify_signed_upload_token(provided_token)
        except RuntimeError:
            signed_identity = None
    if signed_identity is None:
        legacy = _legacy_upload_token()
        if legacy:
            if provided_token != legacy:
                raise HTTPException(status_code=401, detail="Invalid upload token")
        elif _upload_secret():
            raise HTTPException(status_code=401, detail="Invalid upload token")
        else:
            raise HTTPException(status_code=500, detail="Server upload secret/token is not configured")
    if signed_identity is not None:
        if (payload.player_uid.strip() != signed_identity["player_uid"] or
                payload.player_name.strip() != signed_identity["player_name"]):
            raise HTTPException(status_code=401, detail="Upload token identity does not match payload")
    now = _utc_now_iso()
    with _db() as conn:
        cursor = conn.execute(
            """
            INSERT INTO boss_raid_profiles (
                profile_name, description,
                boss_total_hp, enrage_time_s,
                player_uid, player_name,
                profile_json, schema_version,
                created_at, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                payload.profile_name.strip(),
                payload.description.strip(),
                int(payload.boss_total_hp or 0),
                int(payload.enrage_time_s or 600),
                payload.player_uid.strip(),
                payload.player_name.strip(),
                json.dumps(payload.profile, ensure_ascii=False),
                int(payload.schema_version or 1),
                now,
                now,
            ],
        )
        conn.commit()
        new_id = int(cursor.lastrowid)
        row = conn.execute("SELECT * FROM boss_raid_profiles WHERE id = ?", [new_id]).fetchone()
    return _br_row_to_detail(row)


if __name__ == "__main__":
    import uvicorn

    # 直接传 app 对象，避免冻结环境下按模块名导入失败
    uvicorn.run(app, host="0.0.0.0", port=9320, reload=False)
