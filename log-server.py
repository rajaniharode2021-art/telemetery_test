#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import signal
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


DEFAULT_PORT = 8080
MAX_BATCH_SIZE = 1000
MAX_MESSAGE_LEN = 4096

store_lock = threading.Lock()
request_lock = threading.Lock()
request_count = 0


def parse_int_env(name: str, default: int, minimum: int, maximum: int) -> int:
    value = os.environ.get(name)
    if not value:
        return default
    try:
        parsed = int(value)
    except ValueError:
        print(f"invalid {name}={value}; using {default}", file=sys.stderr, flush=True)
        return default
    if minimum <= parsed <= maximum:
        return parsed
    print(f"invalid {name}={value}; using {default}", file=sys.stderr, flush=True)
    return default


def resolve_data_dir() -> Path:
    configured = os.environ.get("LOG_SERVER_DATA_DIR")
    candidates = [Path(configured)] if configured else []
    candidates.extend([Path("/var/lib/log-server"), Path("data"), Path(".")])

    for candidate in candidates:
        try:
            candidate.mkdir(parents=True, exist_ok=True)
        except OSError:
            continue
        if candidate.is_dir():
            return candidate

    raise RuntimeError("failed to create log storage directory")


DATA_DIR = resolve_data_dir()
LOG_FILE = DATA_DIR / "logs.jsonl"
FAILURE_PERCENT = parse_int_env("LOG_SERVER_503_PERCENT", 0, 0, 100)


def should_inject_503() -> bool:
    global request_count

    if FAILURE_PERCENT <= 0:
        return False

    with request_lock:
        if FAILURE_PERCENT >= 100:
            request_count += 1
            return True

        previous_count = request_count
        request_count += 1
        return (request_count * FAILURE_PERCENT // 100) != (
            previous_count * FAILURE_PERCENT // 100
        )


def json_response(handler: BaseHTTPRequestHandler, status: HTTPStatus, body: object) -> None:
    payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(payload)))
    handler.end_headers()
    handler.wfile.write(payload)


def parse_query_int(values: dict[str, list[str]], name: str, default: int) -> int:
    if name not in values:
        return default
    if len(values[name]) != 1:
        raise ValueError(name)
    return int(values[name][0])


def validate_entry(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError("entry must be an object")

    message = value.get("message")
    timestamp = value.get("timestamp")
    if not isinstance(message, str) or len(message) > MAX_MESSAGE_LEN:
        raise ValueError("invalid message")
    if isinstance(timestamp, bool) or not isinstance(timestamp, (int, float)):
        raise ValueError("invalid timestamp")

    return {"message": message, "timestamp": int(timestamp)}


def parse_batch(body: bytes) -> list[dict[str, object]]:
    if not body:
        raise ValueError("missing body")

    payload = json.loads(body.decode("utf-8"))
    if not isinstance(payload, list) or len(payload) > MAX_BATCH_SIZE:
        raise ValueError("invalid batch")

    return [validate_entry(entry) for entry in payload]


def append_entries(entries: list[dict[str, object]]) -> None:
    with store_lock, LOG_FILE.open("a", encoding="utf-8") as file:
        for entry in entries:
            file.write(json.dumps(entry, separators=(",", ":")) + "\n")


def read_entries(since: int, limit: int) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    if not LOG_FILE.exists():
        return entries

    with store_lock, LOG_FILE.open("r", encoding="utf-8") as file:
        for line in file:
            try:
                entry = validate_entry(json.loads(line))
            except (json.JSONDecodeError, ValueError):
                continue
            if int(entry["timestamp"]) < since:
                continue
            if limit > 0 and len(entries) >= limit:
                continue
            entries.append(entry)

    return entries


class LogHandler(BaseHTTPRequestHandler):
    server_version = "log-server/1.0"

    def do_POST(self) -> None:
        if should_inject_503():
            json_response(self, HTTPStatus.SERVICE_UNAVAILABLE, {"error": "service unavailable"})
            return

        parsed = urlparse(self.path)
        if parsed.path != "/batch":
            json_response(self, HTTPStatus.NOT_FOUND, {"error": "not found"})
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            entries = parse_batch(self.rfile.read(content_length))
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError):
            json_response(self, HTTPStatus.BAD_REQUEST, {"error": "invalid batch JSON"})
            return

        try:
            append_entries(entries)
        except OSError:
            json_response(self, HTTPStatus.INTERNAL_SERVER_ERROR, {"error": "storage failed"})
            return

        json_response(self, HTTPStatus.OK, {"status": "ok"})

    def do_GET(self) -> None:
        if should_inject_503():
            json_response(self, HTTPStatus.SERVICE_UNAVAILABLE, {"error": "service unavailable"})
            return

        parsed = urlparse(self.path)
        if parsed.path != "/batch":
            json_response(self, HTTPStatus.NOT_FOUND, {"error": "not found"})
            return

        try:
            query = parse_qs(parsed.query, keep_blank_values=True)
            since = parse_query_int(query, "since", 0)
            limit = parse_query_int(query, "n", 0)
            entries = read_entries(since, limit)
        except (OSError, ValueError):
            json_response(self, HTTPStatus.BAD_REQUEST, {"error": "invalid query params"})
            return

        json_response(self, HTTPStatus.OK, entries)

    def log_message(self, format: str, *args: object) -> None:
        print(f"{self.address_string()} - {format % args}", flush=True)


def main() -> int:
    port = parse_int_env("PORT", DEFAULT_PORT, 1, 65535)
    server = ThreadingHTTPServer(("", port), LogHandler)

    def shutdown(_signum: int, _frame: object) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    print(f"log-server listening on :{port}", flush=True)
    print(f"storing logs at {LOG_FILE}", flush=True)
    print(f"503 injection rate: {FAILURE_PERCENT}%", flush=True)

    try:
        server.serve_forever()
    finally:
        server.server_close()
        print("log-server stopped", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
