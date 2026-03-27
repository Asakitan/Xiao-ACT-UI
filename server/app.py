# -*- coding: utf-8 -*-
"""FastAPI server for SAO auto-key profile repository."""

import json
import os
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


@asynccontextmanager
async def _lifespan(app_: FastAPI):
    _ensure_db()
    yield


app = FastAPI(title="SAO Auto Key Repository", version="1.0.0", lifespan=_lifespan)


class UploadScriptPayload(BaseModel):
    profile_name: str = Field(default="")
    description: str = Field(default="")
    profession_id: int = Field(default=0)
    profession_name: str = Field(default="")
    player_uid: str = Field(default="")
    player_name: str = Field(default="")
    schema_version: int = Field(default=1)
    profile: Dict[str, Any] = Field(default_factory=dict)


def _utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


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


@app.post("/api/scripts")
def upload_script(payload: UploadScriptPayload, x_sao_upload_token: Optional[str] = Header(default="")):
    expected = os.environ.get("SAO_UPLOAD_TOKEN", "").strip()
    if not expected:
        raise HTTPException(status_code=500, detail="Server upload token is not configured")
    if (x_sao_upload_token or "").strip() != expected:
        raise HTTPException(status_code=401, detail="Invalid upload token")
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


if __name__ == "__main__":
    import uvicorn

    # 直接传 app 对象，避免冻结环境下按模块名导入失败
    uvicorn.run(app, host="0.0.0.0", port=9320, reload=False)
