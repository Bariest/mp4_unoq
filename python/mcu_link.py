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
import json
import os
import queue
import socket
import struct
import threading
import time

from arduino.app_utils import Bridge

FW_EXPECTED = 6
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
    @staticmethod
    def _docker_ip(ip):
        """172.16.0.0/12: App Lab's Docker networks, not the board's Wi-Fi / Ethernet address"""
        try:
            a, b = (int(x) for x in ip.split(".")[:2])
        except ValueError:
            return False
        return a == 172 and 16 <= b <= 31

    HOST_NET = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".host_net")

    def _host_net(self):
        """(ip, hostname) of the board itself, from .host_net in the app folder:
        the start script (tools/mpx_start.sh) writes it every 10 s from the board's
        own `hostname -I` and `hostname`, so it follows a Wi-Fi that comes up late
        or a new address. ("", "") when there is no such file."""
        try:
            with open(self.HOST_NET) as f:
                parts = f.read().split()
        except OSError:
            return "", ""
        ip = parts[0] if parts and parts[0].count(".") == 3 else ""
        name = parts[-1] if parts and parts[-1] != ip else ""     # "<ip> <name>", or " <name>" before the Wi-Fi is up
        return ("" if self._docker_ip(ip) else ip), name

    WIFI_STATE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".mpx_run", "wifi_state.json")

    def _wifi_ap(self):
        """the AP's name when the mpx-wifi service has it up (tools/mpx_wifi.py), else "" """
        try:
            with open(self.WIFI_STATE) as f:
                st = json.load(f)
        except (OSError, ValueError):
            return ""
        ap = st.get("ap", {})
        return ap.get("ssid", "") if st.get("mode") == "ap" and time.time() - st.get("t", 0) < 30 else ""

    def _send_net(self, force=False):
        # This runs inside App Lab's container: its own socket address is the
        # container's (172.x) and its hostname the container's id. The board's
        # own come from .host_net (the start script), else App Lab's HOST_IP
        # (fixed when the container started: empty if the Wi-Fi was not up yet).
        ip, name = self._host_net()
        if not ip:
            env = os.environ.get("HOST_IP", "").strip()
            ip = env.split(",")[0].split()[0] if env else ""
        if not ip or self._docker_ip(ip):
            ip = ""
            try:
                sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sk.connect(("10.255.255.255", 1))
                ip = sk.getsockname()[0]
                sk.close()
            except OSError:
                pass
            if self._docker_ip(ip):
                ip = ""                    # better no address than the container's
        ap = self._wifi_ap()
        if ap:                                # the board's own AP: "join MPX-Dog-XXXXXX 192.168.2.1"
            now = (False, "", ap + " 192.168.2.1")
        else:
            now = (bool(ip), ip + ":7000" if ip else "", name or "the board's Wi-Fi")
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
            # nvs: 0 = no settings yet, 2 = started from the copy in its flash
            # (Linux's settings are sent anyway: a difference goes to that copy), 1 = ours
            if h.get("nvs") != 1:
                n = 0
                for key, (typ, raw) in self.nvs.items():
                    try:
                        Bridge.call("mp_nvs", key, typ, raw, timeout=3)
                        n += 1
                    except Exception as e:
                        self.log("could not give the sketch %s: %s" % (key, e))
                Bridge.call("mp_nvs_done", timeout=3)
                self.log("sketch fw %s: %d saved settings sent, %s" % (h.get("fw"), n,
                         "robot already standing (settings from the MCU flash)" if h.get("nvs") == 2
                         else "robot starting"))
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
                if h.get("nvs") != 1:
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

    # ── the IMU's mounting (sketch/imu_remap.h) ──────────────────────────
    def imu(self):
        """{"map": [3 ints], "raw": [ax, ay, az] g before the map, "acc": after it, "gyr": dps after it}"""
        out = {}
        for kv in str(Bridge.call("mp_imu", timeout=3)).split(";"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                out[k] = [int(x) if k == "map" else float(x) for x in v.split(",")]
        return out

    def imu_map(self, a, b, c):
        return bool(Bridge.call("mp_imu_map", int(a), int(b), int(c), timeout=3))
