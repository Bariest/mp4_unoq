"""display_api.py -- the app's Display page (/v1/display), as firmware-dev's
network/http_display.cc answers it, split between the two chips:

  here (Linux)   the GIF files: the three built-in ones (display_gifs/) and the
                 ones you add (<app>/display/), the list, previews, uploads,
                 deletes, the storage figures; and sending the chosen file into
                 the MCU's RAM before it plays (mp_gif_begin / _put / _end)
  the sketch     the screen: GET /v1/display/state, POST /v1/display/set
                 (sketch/mcu_display.cpp), the face and the GIF player

Same JSON and the same messages as the ESP, so the app's page works unchanged.
The MCU has no room for big files: an upload is limited to UPLOAD_MAX."""
import asyncio
import json
import os
import re
import shutil
import threading

from fastapi import Request
from fastapi.responses import Response

from arduino.app_utils import Bridge

HERE = os.path.dirname(os.path.abspath(__file__))
BUILTIN = [("blink", "Blink"), ("smile", "Smile"), ("sleepy", "Sleepy")]
UPLOAD_MAX = 200 * 1024        # what the MCU can hold next to the face (the ESP allowed 600 KB in PSRAM)
MAX_W, MAX_H = 320, 240
CHUNK = 200                    # bytes per Bridge message (256 with the call around it)
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")


def _json(obj, status=200):
    return Response(content=json.dumps(obj, separators=(",", ":")), status_code=status,
                    media_type="application/json", headers={"Cache-Control": "no-store"})


def _err(status, text):
    return Response(content=text, status_code=status, media_type="text/plain")


def gif_size(head):
    if len(head) < 10 or head[:4] != b"GIF8" or head[4:5] not in (b"7", b"9") or head[5:6] != b"a":
        return None
    return head[6] | (head[7] << 8), head[8] | (head[9] << 8)


def pretty(gid):
    out = "".join(" " if c in "_-" else c for c in gid)
    return out[:1].upper() + out[1:]


class Display:
    def __init__(self, link, log, base):
        self.link = link
        self.log = log
        self.builtin_dir = os.path.join(HERE, "display_gifs")
        self.user_dir = os.path.join(base, "display")
        os.makedirs(self.user_dir, exist_ok=True)
        self.push_lock = threading.Lock()
        self.loaded = ""          # what the MCU has in RAM, as far as we know
        link.on_gif_want = self._on_want
        link.on_restart = self._on_restart

    # ── files ─────────────────────────────────────────────────────────────
    def path(self, gid):
        if any(gid == b for b, _ in BUILTIN):
            return os.path.join(self.builtin_dir, gid + ".gif")
        return os.path.join(self.user_dir, gid + ".gif")

    def anim(self, gid, name, builtin):
        p = self.path(gid)
        try:
            with open(p, "rb") as f:
                head = f.read(10)
            size = os.path.getsize(p)
        except OSError:
            return None
        wh = gif_size(head)
        if not wh:
            return None
        return {"id": gid, "name": name, "bytes": size, "w": wh[0], "h": wh[1], "builtin": builtin}

    def anims(self):
        out = [a for a in (self.anim(g, n, True) for g, n in BUILTIN) if a]
        try:
            names = sorted(os.listdir(self.user_dir))
        except OSError:
            names = []
        for fn in names:
            if not fn.lower().endswith(".gif"):
                continue
            gid = fn[:-4]
            if not ID_RE.match(gid) or any(gid == b for b, _ in BUILTIN):
                continue
            a = self.anim(gid, pretty(gid), False)
            if a:
                out.append(a)
        return out

    # ── into the MCU's RAM ────────────────────────────────────────────────
    def push(self, gid):
        """send <gid>.gif to the sketch; True when it is there"""
        with self.push_lock:
            if self.loaded == gid:
                return True
            with open(self.path(gid), "rb") as f:
                data = f.read()
            if not Bridge.call("mp_gif_begin", gid, len(data), timeout=5):
                self.log("display: the MCU has no room for %s.gif (%d bytes)" % (gid, len(data)))
                self.loaded = ""
                return False
            for off in range(0, len(data), CHUNK):
                if not Bridge.call("mp_gif_put", off, data[off:off + CHUNK], timeout=5):
                    self.loaded = ""
                    return False
            ok = bool(Bridge.call("mp_gif_end", gid, timeout=5))
            self.loaded = gid if ok else ""
            if ok:
                self.log("display: %s.gif sent to the MCU (%d bytes)" % (gid, len(data)))
            return ok

    def _on_want(self, gid):
        # from the Bridge's handler thread: no Bridge.call in here
        gid = str(gid)
        if ID_RE.match(gid) and os.path.isfile(self.path(gid)):
            self.loaded = ""          # the MCU says it has not got it
            threading.Thread(target=self._push_quiet, args=(gid,), daemon=True).start()

    def _push_quiet(self, gid):
        try:
            self.push(gid)
        except Exception as e:
            self.log("display: could not send %s.gif: %s" % (gid, e))

    def _on_restart(self):
        self.loaded = ""

    # ── replies ───────────────────────────────────────────────────────────
    def state(self):
        """the MCU's part, or None when it has no screen / is not up"""
        status, _, text = self.link.http("GET", "/v1/display/state")
        if status != 200:
            return None
        st = json.loads(text)
        self.loaded = st.get("loaded", self.loaded) or ""
        return st

    def reply(self, st=None):
        st = st or self.state()
        if not st or not st.get("available"):
            return _err(404, "No screen")
        out = {"on": st["on"], "loop": st["loop"], "id": st["id"], "max_bytes": UPLOAD_MAX,
               "max_w": MAX_W, "max_h": MAX_H}
        for k in ("flip180", "mirror", "shift_x", "shift_y", "brightness", "eyes_alive"):
            out[k] = st[k]
        out["anims"] = self.anims()
        try:
            du = shutil.disk_usage(self.user_dir)
            out["storage"] = {"total": du.total, "used": du.used}
        except OSError:
            pass
        return _json(out)


def register(app, link, log, base):
    d = Display(link, log, base)

    @app.get("/v1/display")
    async def display_get():
        try:
            return await asyncio.to_thread(d.reply)
        except Exception as e:
            return _err(503, "sketch not answering: %s" % e)

    @app.post("/v1/display")
    async def display_post(request: Request):
        raw = (await request.body()).decode("utf-8", "replace")
        try:
            body = json.loads(raw)
            if not isinstance(body, dict):
                raise ValueError
        except ValueError:
            return _err(400, "Invalid JSON")

        def work():
            st = d.state()
            if not st or not st.get("available"):
                return _err(404, "No screen")
            on = body.get("on")
            gid = body.get("id") if isinstance(body.get("id"), str) else ""
            touches = isinstance(on, bool) or gid
            if touches and on is not False:
                gid = gid or st.get("id") or BUILTIN[0][0]
                if not ID_RE.match(gid) or not os.path.isfile(d.path(gid)):
                    return _err(404, "No such animation")
                if not d.push(gid):
                    return _err(500, "Not enough memory to play it right now")
                body["id"] = gid
            status, ctype, text = link.http("POST", "/v1/display/set", json.dumps(body))
            if status != 200:
                return Response(content=text, status_code=status, media_type=ctype or "text/plain")
            return d.reply(json.loads(text))
        try:
            return await asyncio.to_thread(work)
        except Exception as e:
            return _err(503, "sketch not answering: %s" % e)

    @app.get("/v1/display/gif")
    async def gif_get(id: str = ""):
        if not ID_RE.match(id) or not os.path.isfile(d.path(id)):
            return _err(404 if ID_RE.match(id) else 400, "No such animation" if ID_RE.match(id) else "Bad id")
        with open(d.path(id), "rb") as f:
            data = f.read()
        return Response(content=data, media_type="image/gif", headers={"Cache-Control": "max-age=300"})

    @app.post("/v1/display/gif")
    async def gif_post(request: Request, id: str = ""):
        if not ID_RE.match(id):
            return _err(400, "Name it with letters, digits, - or _")
        if any(id == b for b, _ in BUILTIN):
            return _err(400, "That name belongs to a built-in animation")
        data = await request.body()
        if not data or len(data) > UPLOAD_MAX:
            return _err(400, "GIF must be under %d KB on this robot" % (UPLOAD_MAX // 1024))
        wh = gif_size(data[:10])
        if not wh:
            return _err(400, "That is not a GIF")
        if wh[0] > MAX_W or wh[1] > MAX_H:
            return _err(400, "GIF must be 320 x 240 or smaller")
        p = d.path(id)
        with open(p + ".tmp", "wb") as f:
            f.write(data)
        os.replace(p + ".tmp", p)
        if d.loaded == id:
            d.loaded = ""             # the MCU holds the old one
        log("display: animation '%s' added (%d bytes)" % (id, len(data)))
        try:
            return await asyncio.to_thread(d.reply)
        except Exception as e:
            return _err(503, "sketch not answering: %s" % e)

    @app.delete("/v1/display/gif")
    async def gif_delete(id: str = ""):
        if not ID_RE.match(id):
            return _err(400, "Bad id")
        if any(id == b for b, _ in BUILTIN):
            return _err(400, "Built-in animations stay")

        def work():
            st = d.state()
            if st and st.get("on") and st.get("id") == id:
                link.http("POST", "/v1/display/set", '{"on":false}')
            try:
                os.remove(d.path(id))
            except OSError:
                return _err(404, "No such animation")
            log("display: animation '%s' removed" % id)
            return d.reply()
        try:
            return await asyncio.to_thread(work)
        except Exception as e:
            return _err(503, "sketch not answering: %s" % e)

    return d
