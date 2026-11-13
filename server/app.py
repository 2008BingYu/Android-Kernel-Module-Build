import argparse
import hashlib
import json
import os
import sqlite3
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

                          
KEY_PATH = os.environ.get(
    "ICE_KEY", os.path.join(os.path.dirname(__file__), "..", "keys", "ed25519.pem")
)
ADMIN_TOKEN = os.environ.get("ICE_ADMIN_TOKEN", "change-me")
DB_PATH = os.environ.get("ICE_DB", os.path.join(os.path.dirname(__file__), "cards.db"))
MAX_TICKET_DAYS = 7                

                           
def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with db() as conn:
        conn.execute(
            """CREATE TABLE IF NOT EXISTS cards (
                card        TEXT PRIMARY KEY,
                expire_at   INTEGER NOT NULL,
                device      TEXT,
                banned      INTEGER NOT NULL DEFAULT 0,
                created_at  INTEGER NOT NULL,
                last_used   INTEGER
            )"""
        )


def parse_expire(s: str) -> int:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            dt = datetime.strptime(s, fmt).replace(tzinfo=timezone.utc)
            return int(dt.timestamp())
        except ValueError:
            continue
    raise ValueError("时间格式应为 YYYY-MM-DD 或 YYYY-MM-DD HH:MM:SS")


                          
def ed25519_sign(msg: bytes) -> bytes:
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(msg)
        path = f.name
    try:
        out = subprocess.run(
            ["openssl", "pkeyutl", "-sign", "-inkey", KEY_PATH, "-rawin", "-in", path],
            capture_output=True, check=True,
        ).stdout
        return out
    finally:
        os.unlink(path)


def make_ticket(expire_at: int, device: str) -> str:
    dev = hashlib.sha256(device.encode()).hexdigest()[:8]
    exp_hex = f"{expire_at & 0xFFFFFFFF:08x}"
    msg = f"ICE2.{dev}.{exp_hex}".encode()
    sig = ed25519_sign(msg).hex()
    return f"ICE2.{dev}.{exp_hex}.{sig}"


                            
class Handler(BaseHTTPRequestHandler):
    server_version = "ice-ticket/1.0"

    def _json(self, code: int, obj: dict):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n))
        except Exception:
            return {}

    def _admin_ok(self) -> bool:
        return self.headers.get("X-Admin-Token") == ADMIN_TOKEN

    def log_message(self, fmt, *args):
        print("[%s] %s" % (time.strftime("%H:%M:%S"), fmt % args))

                               
    def do_GET(self):
        if self.path == "/v1/ping":
            return self._json(200, {"ok": True, "time": int(time.time())})
        if self.path == "/admin/cards":
            if not self._admin_ok():
                return self._json(403, {"error": "forbidden"})
            with db() as conn:
                rows = [dict(r) for r in conn.execute("SELECT * FROM cards")]
            return self._json(200, {"cards": rows})
        return self._json(404, {"error": "not found"})

                                
    def do_POST(self):
        path = self.path.split("?")[0]
        body = self._body()

        if path == "/v1/ticket":
            return self.handle_ticket(body)

        if not self._admin_ok():
            return self._json(403, {"error": "forbidden"})

        if path == "/admin/cards":
            card = str(body.get("card", "")).strip()
            if not card:
                return self._json(400, {"error": "card required"})
            try:
                exp = parse_expire(str(body.get("expire", "2030-01-01")))
            except ValueError as e:
                return self._json(400, {"error": str(e)})
            now = int(time.time())
            with db() as conn:
                conn.execute(
                    "INSERT OR REPLACE INTO cards(card,expire_at,device,banned,created_at) "
                    "VALUES(?,?,NULL,0,?)",
                    (card, exp, now),
                )
            return self._json(200, {"ok": True, "card": card, "expire_at": exp})

        if path in ("/admin/ban", "/admin/unban"):
            card = str(body.get("card", "")).strip()
            with db() as conn:
                cur = conn.execute(
                    "UPDATE cards SET banned=? WHERE card=?",
                    (1 if path.endswith("ban") else 0, card),
                )
            return self._json(200, {"ok": cur.rowcount > 0})

        if path == "/admin/bind":
            card = str(body.get("card", "")).strip()
            device = str(body.get("device", "")).strip()
            if not device:
                return self._json(400, {"error": "device required"})
            with db() as conn:
                cur = conn.execute("UPDATE cards SET device=? WHERE card=?", (device, card))
            return self._json(200, {"ok": cur.rowcount > 0})

        if path == "/admin/unbind":
            card = str(body.get("card", "")).strip()
            with db() as conn:
                cur = conn.execute("UPDATE cards SET device=NULL WHERE card=?", (card,))
            return self._json(200, {"ok": cur.rowcount > 0})

        if path == "/admin/delete":
            card = str(body.get("card", "")).strip()
            with db() as conn:
                cur = conn.execute("DELETE FROM cards WHERE card=?", (card,))
            return self._json(200, {"ok": cur.rowcount > 0})

        return self._json(404, {"error": "not found"})

                                 
    def handle_ticket(self, body):
        card = str(body.get("card", "")).strip()
        device = str(body.get("device", "")).strip()
        if not card or not device:
            return self._json(400, {"error": "card and device required"})

        now = int(time.time())
        with db() as conn:
            row = conn.execute("SELECT * FROM cards WHERE card=?", (card,)).fetchone()
            if not row:
                return self._json(403, {"error": "card not found"})
            if row["banned"]:
                return self._json(403, {"error": "card banned"})
            if row["expire_at"] <= now:
                return self._json(403, {"error": "card expired"})

            if row["device"] is None:
                conn.execute(
                    "UPDATE cards SET device=?, last_used=? WHERE card=?",
                    (device, now, card),
                )
            elif row["device"] != device:
                return self._json(403, {"error": "device mismatch"})
            else:
                conn.execute("UPDATE cards SET last_used=? WHERE card=?", (now, card))

            expire_at = min(row["expire_at"], now + MAX_TICKET_DAYS * 86400)

        return self._json(200, {"ticket": make_ticket(expire_at, device), "expire_at": expire_at})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    init_db()
    print(f"listening on {args.host}:{args.port}, db={DB_PATH}")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
