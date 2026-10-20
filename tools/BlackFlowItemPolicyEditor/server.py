#!/usr/bin/env python3
"""Local N6 policy draft editor. Does not modify runtime game configuration."""

from __future__ import annotations

import argparse
import json
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parent
POLICY_KEY = "blackflow-n6-item-policy-v1"
BASELINE = "blackflow-n6-2026-10-18-v1"
LOCK = threading.Lock()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8876)
    parser.add_argument("--draft", type=Path, default=ROOT / "draft.json")
    args = parser.parse_args()
    draft_path = args.draft.resolve()

    class Handler(BaseHTTPRequestHandler):
        def send(self, status: int, payload: bytes, content_type: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(payload)

        def send_json(self, status: int, data: dict) -> None:
            self.send(status, json.dumps(data, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

        def do_GET(self) -> None:
            route = urlsplit(self.path).path
            if route in ("/", "/index.html"):
                self.send(200, (ROOT / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif route == "/api/draft":
                try:
                    with LOCK:
                        data = json.loads(draft_path.read_text(encoding="utf-8")) if draft_path.is_file() else {"states": {}}
                    self.send_json(200, data)
                except (OSError, ValueError):
                    self.send_json(500, {"error": "无法读取已保存草稿；未覆盖原文件。"})
            else:
                self.send_json(404, {"error": "not found"})

        def do_POST(self) -> None:
            if urlsplit(self.path).path != "/api/draft":
                self.send_json(404, {"error": "not found"})
                return
            origin = self.headers.get("Origin")
            if origin is not None and origin != "http://" + self.headers.get("Host", ""):
                self.send_json(403, {"error": "仅接受本地编辑页面的保存请求。"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 1_000_000:
                    raise ValueError("草稿大小无效。")
                data = json.loads(self.rfile.read(length).decode("utf-8"))
                states = data.get("states")
                policy = states.get(POLICY_KEY) if isinstance(states, dict) else None
                if data.get("format") != "blackflow-n6-item-policy-session" or not isinstance(policy, dict):
                    raise ValueError("不是本编辑器的策略草稿。")
                if policy.get("schemaVersion") != 1 or policy.get("difficulty") != 6 or policy.get("baseline") != BASELINE:
                    raise ValueError("仅支持当前版本的 N6 草稿。")
                if not all(isinstance(policy.get(key), dict) for key in ("buy", "sale", "loot", "discard", "orders", "notes", "reviewed")):
                    raise ValueError("草稿缺少必要的策略表。")
                data["savedAt"] = datetime.now(timezone.utc).isoformat()
                encoded = json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
                with LOCK:
                    draft_path.parent.mkdir(parents=True, exist_ok=True)
                    temporary = draft_path.with_suffix(draft_path.suffix + ".tmp")
                    temporary.write_text(encoded, encoding="utf-8")
                    temporary.replace(draft_path)
                self.send_json(200, {"savedAt": data["savedAt"]})
            except (ValueError, TypeError, AttributeError) as error:
                self.send_json(400, {"error": str(error)})
            except OSError:
                self.send_json(500, {"error": "保存失败；请保留页面并导出 JSON 备份。"})

        def log_message(self, fmt: str, *args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"BlackFlow N6 policy editor: http://127.0.0.1:{server.server_port}", flush=True)
    print(f"Draft: {draft_path}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
