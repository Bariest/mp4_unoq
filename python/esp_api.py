"""esp_api.py -- the ESP firmware's HTTP API (main/network/http_server.cc), on the UNO Q.

The web app is the ESP's own (pwa-claw, served from assets/ unchanged), so it
calls the same routes it calls on the ESP. Here they are answered as follows:

  /v1/robot/*, /v1/studio/*   forwarded to the sketch, which runs the ESP's own
                              handler code (sketch/mcu_http.cpp): same JSON
  /v1/wifi/*                  the board's Wi-Fi, in the ESP's shapes: status, scan,
                              connect, disconnect, forget, ap-config. Done by the
                              mpx-wifi service on the board (tools/mpx_wifi.py):
                              requests in .mpx_run/wifi_req.json, its state in
                              .mpx_run/wifi_state.json. No "pi" object in status:
                              this robot has no Raspberry Pi link
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

    # ── Wi-Fi: the mpx-wifi service on the board does it (tools/mpx_wifi.py) ──
    run_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".mpx_run")
    wifi_state_path = os.path.join(run_dir, "wifi_state.json")
    wifi_req_path = os.path.join(run_dir, "wifi_req.json")

    def _wifi_state():
        """the service's state, or None when it is not running (not installed)"""
        try:
            with open(wifi_state_path) as f:
                st = json.load(f)
        except (OSError, ValueError):
            return None
        return st if time.time() - st.get("t", 0) < 30 else None

    def _wifi_ask(op, **kw):
        rid = "%d" % (time.time() * 1000)
        tmp = wifi_req_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"id": rid, "op": op, **kw}, f)
        os.replace(tmp, wifi_req_path)
        return rid

    no_helper = {"ok": False, "error": "busy",
                 "detail": "The board's Wi-Fi helper is not running: install the mpx-wifi service (README, Wi-Fi)"}

    @app.get("/v1/wifi/status")
    async def wifi_status(request: Request):
        st = _wifi_state()
        if st is None:
            ip = request.url.hostname or _host_ip()
            return _json({"ap": {"ssid": "", "ip": ""},
                          "sta": {"state": "connected", "ssid": socket.gethostname(), "ip": ip, "error": ""}})
        ap = st.get("ap", {})
        return _json({"ap": {"ssid": ap.get("ssid", "") if ap.get("on") else "", "ip": ap.get("ip", "") if ap.get("on") else ""},
                      "sta": st.get("sta", {})})

    @app.get("/v1/wifi/scan")
    async def wifi_scan():
        if _wifi_state() is None:
            return _json(no_helper)
        rid = _wifi_ask("scan")
        for _ in range(60):                         # the radio takes a few seconds
            await asyncio.sleep(0.5)
            st = _wifi_state() or {}
            if st.get("req_done") == rid:
                return _json({"ok": True, "networks": st.get("scan", [])})
        return _json({"ok": False, "error": "busy"})

    async def _wifi_body(request):
        try:
            return await request.json()
        except Exception:
            return {}

    @app.post("/v1/wifi/connect")
    async def wifi_connect(request: Request):
        b = await _wifi_body(request)
        if not b.get("ssid"):
            return Response("missing 'ssid' field", status_code=400)
        if _wifi_state() is None:
            return _json(no_helper)
        _wifi_ask("connect", ssid=b["ssid"], password=b.get("password", ""), identity=b.get("identity", ""))
        log("Wi-Fi: joining %s" % b["ssid"])
        return _json({"ok": True})

    @app.post("/v1/wifi/disconnect")
    async def wifi_disconnect():
        if _wifi_state() is None:
            return _json(no_helper)
        _wifi_ask("disconnect")
        return _json({"ok": True})

    @app.post("/v1/wifi/forget")
    async def wifi_forget():
        if _wifi_state() is None:
            return _json(no_helper)
        _wifi_ask("forget")
        return _json({"ok": True})

    @app.post("/v1/wifi/ap-config")
    async def wifi_ap_config(request: Request):
        b = await _wifi_body(request)
        if not b.get("ssid"):
            return Response("missing 'ssid' field", status_code=400)
        if _wifi_state() is None:
            return _json(no_helper)
        _wifi_ask("ap-config", ssid=b["ssid"], password=b.get("password", ""))
        return _json({"ok": True, "restarting": False})

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

    # ── the IMU's mounting: which way the BMI270 sits on this carrier board ──
    # Everything on the robot reads the IMU in firmware-dev's ESP board frame:
    # standing flat it reads acc about (0, 0, -1) g; nose down, acc y goes
    # negative; left side down, acc x goes negative. /unoq/imu/learn finds the
    # map from two poses (flat, then nose down) and saves it (imu.map).
    imu_learn = {}

    def _imu_avg(n=10):
        s = [0.0, 0.0, 0.0]
        for _ in range(n):
            r = link.imu()["raw"]
            for i in range(3):
                s[i] += r[i] / n
            time.sleep(0.05)
        return [round(v, 3) for v in s]

    def _imu_check(acc):
        x, y, z = acc
        if z < -0.8:
            return "reads upright (acc z about -1 g): right when the robot stands flat"
        k = max(range(3), key=lambda i: abs(acc[i]))
        return ("NOT upright: gravity is on acc %s%s (should be on z, about -1 g, when the robot stands flat); "
                "run /unoq/imu/learn?pose=flat, then ?pose=nose_down" % ("-" if acc[k] < 0 else "+", "xyz"[k]))

    @app.get("/unoq/imu")
    async def unoq_imu():
        try:
            d = await asyncio.to_thread(link.imu)
        except Exception as e:
            return _json({"ok": False, "error": str(e)})
        return _json({"ok": True, **d, "check": _imu_check(d.get("acc", [0, 0, 0]))})

    @app.get("/unoq/imu/map")
    async def unoq_imu_map(m: str):
        try:
            a, b, c = (int(v) for v in m.split(","))
        except ValueError:
            return _json({"ok": False, "error": "m=x,y,z: each +-1, +-2 or +-3, e.g. m=1,2,3"})
        ok = await asyncio.to_thread(link.imu_map, a, b, c)
        return _json({"ok": ok, "map": [a, b, c]})

    @app.get("/unoq/imu/learn")
    async def unoq_imu_learn(pose: str):
        raw = await asyncio.to_thread(_imu_avg)
        if pose == "flat":
            imu_learn["flat"] = raw
            return _json({"ok": True, "pose": "flat", "raw": raw,
                          "next": "lift the BACK of the robot so the nose points down about 30-45 deg, hold it "
                                  "still, then open /unoq/imu/learn?pose=nose_down"})
        if pose != "nose_down":
            return _json({"ok": False, "error": "pose=flat first, then pose=nose_down"})
        flat = imu_learn.get("flat")
        if not flat:
            return _json({"ok": False, "error": "do pose=flat first"})
        k = max(range(3), key=lambda i: abs(flat[i]))
        if abs(flat[k]) < 0.8:
            return _json({"ok": False, "error": "flat reading %s: not lying level / still; do pose=flat again" % flat})
        d = [raw[i] - flat[i] for i in range(3)]
        j = max((i for i in range(3) if i != k), key=lambda i: abs(d[i]))
        if abs(d[j]) < 0.25:
            return _json({"ok": False, "error": "the nose is not down enough (change %s): tilt about 30-45 deg" % d})
        sz = -1 if flat[k] > 0 else 1          # out z = sz * raw[k]  -> -1 g standing flat
        sy = -1 if d[j] > 0 else 1             # out y = sy * raw[j]  -> negative nose down
        Y = [0, 0, 0]; Y[j] = sy
        Z = [0, 0, 0]; Z[k] = sz
        X = [Y[1] * Z[2] - Y[2] * Z[1], Y[2] * Z[0] - Y[0] * Z[2], Y[0] * Z[1] - Y[1] * Z[0]]   # x = y cross z
        i = max(range(3), key=lambda n: abs(X[n]))
        m = [int(X[i]) * (i + 1), sy * (j + 1), sz * (k + 1)]
        ok = await asyncio.to_thread(link.imu_map, *m)
        log("IMU map learned: %s (flat %s, nose down %s)" % (m, flat, raw))
        imu_learn.clear()
        return _json({"ok": ok, "map": m, "flat": flat, "nose_down": raw,
                      "next": "put it down flat: /unoq/imu should now say it reads upright"})

    @app.get("/unoq/log")
    async def unoq_log():
        return _json({"ok": True, "mcu": list(link.mcu_log)[-200:], "app": list(log.lines)[-100:]})

    for add in extra:                  # display_api (/v1/display), policy_api (/v1/policy)
        add(app)

    # Whatever the WebUI version mounted at "/" already (the assets) must come
    # after these, or it would answer /v1/... with a 404 of its own.
    ours = [r for r in app.router.routes if r not in before]
    app.router.routes[:] = ours + before
