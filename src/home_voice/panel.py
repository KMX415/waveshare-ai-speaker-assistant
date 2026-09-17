"""Loopback-only PC setup. Credentials are sent to device storage, never logs."""
import asyncio
import hmac
import json
import os
from pathlib import Path
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .bridge import serve_bridge
from .config import Settings
from .usb import BoardLink


class Controller:
    def __init__(self, port):
        self.board = BoardLink(port)
        self.settings = Settings.from_env(mock=True)
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.bridge = asyncio.run_coroutine_threadsafe(serve_bridge(self.settings), self.loop)
        self.session = None
        self.message = "Ready for a local audio test."

    def busy(self):
        return self.board.native.get("active",False) or (self.session is not None and not self.session.done())

    def status(self):
        if self.board.info.get("standalone"):
            native=self.board.native
            return dict(device=self.board.info,levels=self.board.levels,active=self.busy(),
                        key_set=native.get("key_set",False),key_saved=native.get("key_saved",False),
                        secure_storage=native.get("secure_storage",False),wifi_connected=native.get("wifi_connected",False),
                        wake=getattr(self.board,"wake",{}),failure=getattr(self.board,"failure",{}),playback=getattr(self.board,"playback",{}),upload=getattr(self.board,"upload",{}),crash=list(getattr(getattr(self.board,"crash",None),"lines",[])),mock=False,message=self.board.error or native.get("message","Waiting for device"),volume=self.board.volume,
                        result={"type":"closed","finalized":native.get("finalized",False),"usage":{"seconds":native.get("seconds")}} if native.get("finalized") else {})
        error = self.board.error
        message = self.message
        if self.bridge.done():
            error = "Voice service failed to start. Check whether port 8765 is already in use."
        if self.session and self.session.done():
            try:
                self.session.result()
                if self.board.result.get("type") == "closed":
                    message = "Session closed. Ready for another conversation."
            except Exception:
                error = error or "Conversation could not start or lost its connection. Check API access and billing."
        return dict(device=self.board.info, levels=self.board.levels, active=self.busy(),
                    key_set=bool(self.settings.api_key), mock=self.settings.mock,
                    message=error or message, result=self.board.result, volume=self.board.volume)

    def action(self, command, data):
        if command == "key":
            if self.busy(): raise ValueError("Stop the conversation before changing the key.")
            key = data.get("key", "")
            if not isinstance(key, str) or not 20 <= len(key.strip()) <= 512:
                raise ValueError("Enter a valid OpenAI API key.")
            if self.board.info.get("standalone"):
                if not key.strip().isascii() or any(ord(c)<33 for c in key.strip()):raise ValueError("Invalid key format")
                self.board.save_key(key.strip())
                return {"ok":True}
            self.settings.api_key = key.strip()
            self.message = "API key loaded for this run. Start a live conversation to check access."
        elif command == "start":
            if self.busy(): raise ValueError("A conversation is already active.")
            if not self.board.info: raise ValueError("Waiting for the Home Voice board.")
            if self.board.info.get("standalone") and data.get("mock") is not True:
                if not self.board.native.get("key_set"):raise ValueError("Load the API key on the device first.")
                if not self.board.native.get("wifi_connected"):raise ValueError("Configure the device Wi-Fi first.")
                self.board.command(b"G")
                return {"ok":True}
            if self.bridge.done(): raise ValueError("The voice service is unavailable.")
            self.settings.mock = data.get("mock") is True
            if not self.settings.mock and not self.settings.api_key:
                raise ValueError("Enter your API key first.")
            self.session = asyncio.run_coroutine_threadsafe(
                self.board.converse("ws://127.0.0.1:8765/audio", self.settings.device_token), self.loop)
            self.message = "Local audio test running." if self.settings.mock else "Live conversation running."
        elif command == "stop":
            if self.board.info.get("standalone"):self.board.command(b"Z")
            self.board.stop.set()
            self.message = "Stopping and collecting final session usage…"
        elif command == "wake":
            if self.busy():raise ValueError("Stop the conversation before changing wake-word settings.")
            self.board.configure_wake(data)
        elif command == "volume":
            self.board.set_volume(data.get("volume"))
        elif command == "tone":
            if self.busy(): raise ValueError("Stop the conversation before playing a test tone.")
            self.board.command(b"T")
        elif command == "setup":
            if self.busy(): raise ValueError("Stop the conversation before opening Wi-Fi setup.")
            self.board.setup = {}
            self.board.command(b"C")
            return {"ok": True}
        else:
            raise ValueError("Unknown action")
        return {"ok": True}

    def close(self):
        self.board.stop.set()
        if self.session:
            try: self.session.result(timeout=20)
            except Exception: self.session.cancel()
        self.board.close()
        self.bridge.cancel()
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=2)


def create_panel_server(controller, http_port=8766):
    token = secrets.token_urlsafe(32)
    page = Path(__file__).with_name("panel.html").read_text(encoding="utf-8").replace("__CSRF__", token)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args): pass

        def permitted(self, mutate=False):
            if self.headers.get("Host") not in (f"127.0.0.1:{http_port}", f"localhost:{http_port}"):
                return False
            if mutate:
                if self.headers.get("Origin") not in (f"http://127.0.0.1:{http_port}", f"http://localhost:{http_port}"):
                    return False
                if not hmac.compare_digest(self.headers.get("X-Home-Voice", ""), token):
                    return False
            return True

        def respond(self, value, status=200):
            content = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def do_GET(self):
            if not self.permitted(): return self.respond({"error":"Local access only"},403)
            if self.path == "/":
                content = page.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Frame-Options", "DENY")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            elif self.path == "/api/status": self.respond(controller.status())
            elif self.path == "/api/setup": self.respond(controller.board.setup)
            else: self.respond({"error":"Not found"},404)

        def do_POST(self):
            if not self.permitted(True): return self.respond({"error":"Request rejected"},403)
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 2048: raise ValueError("Invalid request size")
                data = json.loads(self.rfile.read(length))
                if not isinstance(data,dict): raise ValueError("Expected an object")
                result = controller.action(self.path.removeprefix("/api/"),data)
                self.respond(result)
            except ValueError as exc: self.respond({"error":str(exc)},400)
            except Exception: self.respond({"error":"Device action failed. Check the USB connection."},500)

    server = ThreadingHTTPServer(("127.0.0.1",http_port),Handler)
    http_port = server.server_address[1]
    server.daemon_threads = True
    return server


def serve_panel(port="COM14", http_port=8766):
    controller = Controller(port)
    try:
        server = create_panel_server(controller, http_port)
    except Exception:
        controller.close()
        raise
    print(f"PC setup: http://127.0.0.1:{http_port}/",flush=True)
    try: server.serve_forever()
    finally:
        server.server_close()
        controller.close()
