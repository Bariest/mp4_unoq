"""mcu_link.py -- the Bridge to the sketch, in one place.

The sketch runs the ESP firmware's movement code and its /v1/robot + /v1/studio
HTTP handlers (sketch/mcu_http.cpp). This module:
  * forwards an HTTP request to it and reassembles the reply (mp_http +
    mp_http_more: a Bridge message is 256 bytes, a reply can be several KB);
  * gives it the saved settings at boot (NVS, see nvs_store.py) and saves what it
    changes (mp_nvs_save);
  * feeds the TrajOpt / Character move rows it asks for (mp_tj_want ->
    mp_tj_push), from tj_tables.bin;
  * collects its log lines (mp_log);
  * tells it the address the web app is on (mp_net: the line under the eyes) and
    passes its requests for a GIF to display_api.py (mp_gif_want).
"""
import collections
import os
import queue
import socket
import struct
import threading
import time

from arduino.app_utils import Bridge

FW_EXPECTED = 5
HERE = os.path.dirname(os.path.abspath(__file__))


def parse_hello(s):
    out = {}
    for kv in str(s).split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


class TjTables:
    """python/tj_tables.bin: the rows of every TrajOpt / Character table (port_from_esp.py)"""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        if data[:6] != b"MPXTJ1":
            raise ValueError("not a tj_tables.bin")
        (n,) = struct.unpack_from("<H", data, 6)
        self.tables = []
        for k in range(n):
            off, rows = struct.unpack_from("<II", data, 8 + 8 * k)
            self.tables.append(data[off:off + 24 * rows])

    def rows(self, k, start, count):
        t = self.tables[k]
        return t[24 * start:24 * (start + count)]

    def last(self, k):
        return self.tables[k][-24:]

    def count(self, k):
        return len(self.tables[k]) // 24


class McuLink:
    def __init__(self, nvs, log):
        self.nvs = nvs
        self.log = log
        self.mcu_log = collections.deque(maxlen=400)
        self.http_lock = threading.Lock()
        self.hello = {}
        self.started = False
        self.tj = None
        try:
            self.tj = TjTables(os.path.join(HERE, "tj_tables.bin"))
        except (OSError, ValueError) as e:
            self.log("no TrajOpt rows (%s): the Character moves and spin jumps will be skipped" % e)
        self._tj_q = queue.Queue()
        self.on_gif_want = None        # display_api.Display sets these
        self.on_restart = None
        self._net_last = None
        Bridge.provide("mp_log", self._on_log)
        Bridge.provide("mp_gif_want", self._on_gif_want)
        Bridge.provide("mp_nvs_save", self._on_nvs_save)
        Bridge.provide("mp_tj_want", self._on_tj_want)
        threading.Thread(target=self._tj_worker, name="tj-rows", daemon=True).start()
        threading.Thread(target=self._boot, name="mcu-boot", daemon=True).start()

    # ── from the sketch (Bridge handlers: no Bridge.call in here) ─────────
    def _on_log(self, line):
        line = str(line)
        self.mcu_log.append(time.strftime("%H:%M:%S ") + line)
        print("[mcu] " + line, flush=True)

    def _on_nvs_save(self, key, typ, value):
        self.nvs.put(str(key), str(typ), bytes(value))

    def _on_gif_want(self, gid):
        if self.on_gif_want:
            self.on_gif_want(str(gid))

    def _on_tj_want(self, table, next_row, space):
        self._tj_q.put((int(table), int(next_row), int(space)))

    # ── the address under the eyes ────────────────────────────────────────
    def _send_net(self, force=False):
        ip = ""
        try:
            sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sk.connect(("10.255.255.255", 1))
            ip = sk.getsockname()[0]
            sk.close()
        except OSError:
            pass
        now = (bool(ip), ip + ":7000" if ip else "", socket.gethostname())
        if force or now != self._net_last:
            try:
                Bridge.call("mp_net", now[0], now[1], now[2], timeout=3)
                self._net_last = now
            except Exception:
                pass

    # ── boot: settings to the sketch, then it starts the robot ────────────
    def _boot(self):
        while True:
            try:
                h = parse_hello(Bridge.call("mp_hello", timeout=3))
            except Exception as e:     # the sketch is still booting / being flashed
                time.sleep(1.0)
                continue
            self.hello = h
            self._send_net(force=True)
            if self.on_restart:
                self.on_restart()
            if h.get("fw") != FW_EXPECTED:
                self.log("WARNING: sketch fw %s, this python expects %d: run the app again so both match"
                         % (h.get("fw"), FW_EXPECTED))
            if not h.get("nvs"):
                n = 0
                for key, (typ, raw) in self.nvs.items():
                    try:
                        Bridge.call("mp_nvs", key, typ, raw, timeout=3)
                        n += 1
                    except Exception as e:
                        self.log("could not give the sketch %s: %s" % (key, e))
                Bridge.call("mp_nvs_done", timeout=3)
                self.log("sketch fw %s: %d saved settings sent, robot starting" % (h.get("fw"), n))
            # watch for a sketch restart (it would wait for the settings again)
            while True:
                time.sleep(3.0)
                try:
                    h = parse_hello(Bridge.call("mp_hello", timeout=3))
                except Exception:
                    continue
                self._send_net()
                self.hello = h
                self.started = bool(h.get("started"))
                if not h.get("nvs"):
                    self.log("the sketch restarted: sending the settings again")
                    break

    # ── TrajOpt rows ──────────────────────────────────────────────────────
    def _tj_worker(self):
        while True:
            table, nxt, space = self._tj_q.get()
            # the sketch asks every 15 ms: answer only the newest request
            try:
                while True:
                    table, nxt, space = self._tj_q.get_nowait()
            except queue.Empty:
                pass
            if self.tj is None or not (0 <= table < len(self.tj.tables)):
                continue
            try:
                if nxt < 0:
                    Bridge.call("mp_tj_push", table, -1, self.tj.last(table), timeout=2)
                    continue
                total = self.tj.count(table)
                sent = 0
                while sent < space and nxt < total:
                    n = min(9, space - sent, total - nxt)     # 9 rows = 216 bytes per message
                    got = int(Bridge.call("mp_tj_push", table, nxt, self.tj.rows(table, nxt, n), timeout=2))
                    if got <= 0:
                        break
                    nxt += got
                    sent += got
            except Exception as e:
                self.log("TrajOpt rows: %s" % e)

    # ── HTTP to the sketch ────────────────────────────────────────────────
    def http(self, method, uri, body=""):
        """-> (status, content_type, body_text)"""
        with self.http_lock:
            first = str(Bridge.call("mp_http", method, uri, body or "", timeout=15))
            parts = first.split("|", 3)
            if len(parts) != 4:
                return 502, "text/plain", "bad reply from the sketch"
            status, ctype, total, inline = int(parts[0]), parts[1], int(parts[2]), parts[3]
            if len(inline) == total:
                return status, ctype, inline
            data = b""
            while len(data) < total:
                more = bytes(Bridge.call("mp_http_more", len(data), timeout=5))
                if not more:
                    break
                data += more
            return status, ctype, data.decode("utf-8", "replace")

    def spi(self, edge=-1, khz=0):
        return parse_hello(Bridge.call("mp_spi", int(edge), int(khz), timeout=3))
