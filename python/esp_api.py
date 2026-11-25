"""esp_api.py -- the ESP firmware's HTTP API (main/network/http_server.cc), on the UNO Q.

The web app is the ESP's own (pwa-claw, served from assets/ unchanged), so it
calls the same routes it calls on the ESP. Here they are answered as follows:

  /v1/robot/*, /v1/studio/*   forwarded to the sketch, which runs the ESP's own
                              handler code (sketch/mcu_http.cpp): same JSON
  /v1/wifi/status             this board's network, in the ESP's shape (no "pi"
                              object: this robot has no Raspberry Pi link, and the
                              app hides that section, as for any robot without one)
  /v1/wifi/scan|connect|...   refused: the UNO Q's Wi-Fi is set up in App Lab
  /v1/system/stats            Linux CPU and memory, in sys_monitor.cc's shape
  /v1/agent/permissions       kept, as on the ESP
  /v1/chat/ui (WebSocket)     accepted and quiet: no AI agent on this robot yet
  everything else             404, which the app reads as "this firmware does not
                              have it" and hides (AI, display, voice, skills, RL...)
"""
import asyncio
import json
import os
import socket
import time

from fastapi import Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse, Response


def _json(obj, status=200):
    return JSONResponse(obj, status_code=status, headers={"Cache-Control": "no-store"})


class SysStats:
    """/proc -> the fields SystemMonitor.svelte reads (cpu per core, memory, tasks)"""

    def __init__(self):
        self.prev = None
        self.prev_t = time.monotonic()
        self.t0 = time.monotonic()

    @staticmethod
    def _cpu_times():
        out = []
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("cpu") and line[3].isdigit():
                    v = [int(x) for x in line.split()[1:]]
                    idle = v[3] + (v[4] if len(v) > 4 else 0)
                    out.append((sum(v), idle))
        return out

    def json(self):
        now = time.monotonic()
        try:
            cur = self._cpu_times()
        except OSError:
            cur = []
        cpu = []
        if self.prev and len(self.prev) == len(cur):
            for (t0, i0), (t1, i1) in zip(self.prev, cur):
                dt = t1 - t0
                cpu.append(round(100 * (1 - (i1 - i0) / dt)) if dt > 0 else 0)
        else:
            cpu = [0 for _ in cur]
        window_ms = int((now - self.prev_t) * 1000)
        self.prev, self.prev_t = cur, now
        mem = {}
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    k, v = line.split(":", 1)
                    mem[k] = int(v.split()[0]) * 1024
        except (OSError, ValueError):
            pass
        total = mem.get("MemTotal", 0)
        free = mem.get("MemAvailable", mem.get("MemFree", 0))
        try:
            with open("/proc/uptime") as f:
                up = int(float(f.read().split()[0]))
        except (OSError, ValueError):
            up = int(now - self.t0)
        try:
            nthreads = len(os.listdir("/proc/self/task"))
        except OSError:
            nthreads = 0
        return {
            "ok": True, "uptime_s": up, "window_ms": window_ms, "cpu": cpu,
            "internal": {"total": total, "free": free, "min_free": free, "largest": free},
            "psram": {"total": 0, "free": 0, "min_free": 0, "largest": 0},
            "tasks": {"count": nthreads, "top": [{"name": "mpx app (python)", "core": -1, "cpu": 0}],
                      "low_stack": {"name": "-", "stack_free": 65536}},
        }


def _host_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return ""


def register(app, link, settings, log, extra=()):
    """add the routes to the WebUI's FastAPI app, in front of its static files"""
    stats = SysStats()
    before = list(app.router.routes)

    async def forward(request: Request, path: str):
        body = (await request.body()).decode("utf-8", "replace")
        uri = "/v1/" + path + (("?" + request.url.query) if request.url.query else "")
        try:
            status, ctype, text = await asyncio.to_thread(link.http, request.method, uri, body)
        except Exception as e:
            return _json({"ok": False, "error": "sketch not answering: %s" % e}, 503)
        return Response(content=text, status_code=status, media_type=ctype or "application/json",
                        headers={"Cache-Control": "no-store"})

    @app.api_route("/v1/robot/{rest:path}", methods=["GET", "POST"])
    async def robot_routes(request: Request, rest: str):
        return await forward(request, "robot/" + rest)

    @app.api_route("/v1/policy", methods=["GET", "POST"])
    async def policy_root(request: Request):
        return await forward(request, "policy")

    @app.api_route("/v1/policy/{rest:path}", methods=["GET", "POST"])
    async def policy_routes(request: Request, rest: str):
        return await forward(request, "policy/" + rest)

    @app.api_route("/v1/studio/{rest:path}", methods=["GET", "POST"])
    async def studio_routes(request: Request, rest: str):
        return await forward(request, "studio/" + rest)

    @app.get("/v1/wifi/status")
    async def wifi_status(request: Request):
        ip = request.url.hostname or _host_ip()
        return _json({"ap": {"ssid": "", "ip": ""},
                      "sta": {"state": "connected", "ssid": socket.gethostname(), "ip": ip}})

    @app.api_route("/v1/wifi/{rest:path}", methods=["GET", "POST"])
    async def wifi_other(rest: str):
        return _json({"ok": False, "error": "busy",
                      "detail": "The UNO Q's Wi-Fi is set up in Arduino App Lab, not from this app"})

    # the controller layout every browser of this robot shares (http_server.cc keeps it in NVS "ui")
    @app.get("/v1/settings/control-scheme")
    async def scheme_get():
        return _json({"ok": True, "scheme": "Haris" if settings.get("control_scheme") == "Haris" else "PS4"})

    @app.post("/v1/settings/control-scheme")
    async def scheme_post(request: Request):
        try:
            scheme = json.loads((await request.body()).decode("utf-8", "replace")).get("scheme")
        except (ValueError, AttributeError):
            scheme = None
        if scheme not in ("PS4", "Haris"):
            return Response("expected scheme PS4 or Haris", status_code=400)
        settings["control_scheme"] = scheme
        settings.save()
        return _json({"ok": True, "scheme": scheme})

    @app.get("/v1/system/stats")
    async def system_stats():
        return _json(await asyncio.to_thread(stats.json))

    @app.get("/v1/agent/permissions")
    async def perms_get():
        return _json({"ok": True, "allow_writes": bool(settings.get("allow_writes", False))})

    @app.post("/v1/agent/permissions")
    async def perms_post(request: Request):
        body = (await request.body()).decode("utf-8", "replace")
        on, off = '"allow_writes":true' in body.replace(" ", ""), '"allow_writes":false' in body.replace(" ", "")
        if on == off:
            return Response('expected {"allow_writes":true|false}', status_code=400)
        settings["allow_writes"] = on
        settings.save()
        return _json({"ok": True, "allow_writes": on})

    @app.websocket("/v1/chat/ui")
    async def chat_ws(ws: WebSocket):
        # The app keeps this socket open for permission prompts from the AI agent,
        # which this robot does not run: accept it and stay quiet.
        await ws.accept()
        try:
            while True:
                await ws.receive_text()
        except WebSocketDisconnect:
            pass
        except Exception:
            pass

    # ── this port's own diagnostics (not ESP routes) ────────────────────
    @app.get("/unoq/hello")
    async def unoq_hello():
        try:
            h = await asyncio.to_thread(lambda: link.hello)
        except Exception as e:
            h = {"error": str(e)}
        return _json({"ok": True, "hello": h})

    @app.get("/unoq/spi")
    async def unoq_spi(edge: int = -1, khz: int = 0):
        return _json({"ok": True, "hello": await asyncio.to_thread(link.spi, edge, khz)})

    @app.get("/unoq/log")
    async def unoq_log():
        return _json({"ok": True, "mcu": list(link.mcu_log)[-200:], "app": list(log.lines)[-100:]})

    for add in extra:                  # display_api (/v1/display), policy_api (/v1/policy)
        add(app)

    # Whatever the WebUI version mounted at "/" already (the assets) must come
    # after these, or it would answer /v1/... with a 404 of its own.
    ours = [r for r in app.router.routes if r not in before]
    app.router.routes[:] = ours + before
