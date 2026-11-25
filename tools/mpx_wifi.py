#!/usr/bin/env python3
"""mpx_wifi.py -- the UNO Q's Wi-Fi for the robot app, like firmware-dev's ESP

Runs on the board itself, as root (tools/mpx-wifi.service), because the robot
app runs in App Lab's container and cannot touch the board's network.

What it does, as the ESP did:
  * Its own network when there is no other: if the board has not joined a
    saved Wi-Fi a minute after the start (or loses it), it opens the access
    point "MPX-Dog-XXXXXX" (open, no password, XXXXXX from the board's
    machine-id: its Wi-Fi MAC is new at every boot) at 192.168.2.1. A phone joins it and opens http://192.168.2.1 (port 80
    is passed on to the app's 7000). The screen shows "join MPX-Dog-XXXXXX".
  * The web app's Wi-Fi page: scan, connect (home / phone hotspot, or WPA2/3
    Enterprise with a login), disconnect, forget, and the AP's name/password.
    The app (python/esp_api.py) leaves a request in .mpx_run/wifi_req.json;
    this answers in .mpx_run/wifi_state.json.
  * .host_net: the board's own address and name for the line under the eyes
    (python/mcu_link.py), every few seconds.

With the AP up, nobody on it and 3 minutes gone, it takes the AP down for
half a minute so the saved networks can come back (a router that was off).

NetworkManager does the work (nmcli). The AP needs dnsmasq for its DHCP
(dnsmasq-base); without it the AP cannot start and the state says so.
"""
import json
import os
import re
import shutil
import subprocess
import time

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(APP, ".mpx_run")
STATE = os.path.join(RUN, "wifi_state.json")
REQ = os.path.join(RUN, "wifi_req.json")
AP_CFG = os.path.join(RUN, "wifi_ap.json")
HOST_NET = os.path.join(APP, ".host_net")

AP_CON = "mpx-ap"
AP_IP = "192.168.2.1"
AP_PREFIX = "MPX-Dog"
BOOT_GRACE_S = 60        # after the start: the saved networks first
LOST_GRACE_S = 30        # a saved network dropped: wait this long before the AP
RETRY_EVERY_S = 180      # AP up with nobody on it: try the saved networks again
RETRY_WAIT_S = 30

try:
    _st = os.stat(APP)
    OWNER = (_st.st_uid, _st.st_gid)   # the files stay the app folder owner's (arduino)
except OSError:
    OWNER = None


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def sh(*args, timeout=30):
    try:
        p = subprocess.run(list(args), capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, "", str(e)


def nm(*args, timeout=30):
    return sh("nmcli", *args, timeout=timeout)


def split_t(line):
    """one line of `nmcli -t`: fields split on ':' that is not escaped"""
    return [f.replace("\\:", ":").replace("\\\\", "\\") for f in re.split(r"(?<!\\):", line)]


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    if OWNER:
        try:
            os.chown(tmp, *OWNER)
        except OSError:
            pass
    os.replace(tmp, path)


def docker_ip(ip):
    try:
        a, b = (int(x) for x in ip.split(".")[:2])
    except ValueError:
        return False
    return a == 172 and 16 <= b <= 31


class Wifi:
    def __init__(self):
        self.dev = ""
        self.t0 = time.monotonic()
        self.lost_at = None             # when the board last had no network
        self.ap_since = None
        self.connecting = False
        self.error = ""
        self.scan = []
        self.scan_t = 0.0
        self.req_done = ""
        self.ap_error = ""
        os.makedirs(RUN, exist_ok=True)
        if OWNER:
            try:
                os.chown(RUN, *OWNER)
            except OSError:
                pass

    # ── what NetworkManager says ────────────────────────────────────────
    def find_dev(self):
        rc, out, _ = nm("-t", "-f", "DEVICE,TYPE", "device")
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[1] == "wifi":
                return f[0]
        return ""

    def dev_state(self):
        """(state, connection) of the Wi-Fi device"""
        rc, out, _ = nm("-t", "-f", "DEVICE,STATE,CONNECTION", "device")
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 3 and f[0] == self.dev:
                return f[1], f[2]
        return "unavailable", ""

    def other_network(self):
        """the board has a network that is not our AP: Wi-Fi or Ethernet"""
        rc, out, _ = nm("-t", "-f", "TYPE,STATE,CONNECTION", "device")
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 3 and f[1] == "connected" and f[2] != AP_CON and f[0] in ("wifi", "ethernet"):
                return True
        return False

    def sta_ssid(self):
        rc, out, _ = nm("-t", "-f", "ACTIVE,SSID", "device", "wifi", "list", "ifname", self.dev, "--rescan", "no")
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[0] == "yes":
                return f[1]
        return ""

    def dev_ip(self):
        rc, out, _ = nm("-g", "IP4.ADDRESS", "device", "show", self.dev)
        for a in out.replace("|", "\n").split():
            if "/" in a:
                return a.split("/")[0]
        return ""

    def ap_on(self):
        state, con = self.dev_state()
        return con == AP_CON and state == "connected"

    def ap_ssid(self):
        cfg = self.ap_cfg()
        if cfg.get("ssid"):
            return cfg["ssid"]
        # The UNO Q's Wi-Fi gets a new random MAC at every boot, so (unlike the
        # ESP) the name comes from the board's machine-id: the same every boot.
        try:
            with open("/etc/machine-id") as f:
                mid = f.read().strip()
            if len(mid) >= 6:
                return "%s-%s" % (AP_PREFIX, mid[:6].upper())
        except OSError:
            pass
        try:
            with open("/sys/class/net/%s/address" % self.dev) as f:
                mac = f.read().strip().replace(":", "").upper()
            return "%s-%s" % (AP_PREFIX, mac[-6:])
        except OSError:
            return AP_PREFIX

    def ap_cfg(self):
        try:
            with open(AP_CFG) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    # ── the access point ────────────────────────────────────────────────
    def ap_up(self):
        if not shutil.which("dnsmasq"):
            self.ap_error = "dnsmasq is missing: sudo apt install dnsmasq-base"
            return False
        self.refresh_scan()                     # the AP takes the radio: scan first
        ssid = self.ap_ssid()
        pw = self.ap_cfg().get("password", "")
        nm("connection", "delete", "id", AP_CON)
        args = ["connection", "add", "type", "wifi", "ifname", self.dev, "con-name", AP_CON,
                "autoconnect", "no", "ssid", ssid,
                "802-11-wireless.mode", "ap", "802-11-wireless.band", "bg",
                "ipv4.method", "shared", "ipv4.addresses", AP_IP + "/24", "ipv6.method", "disabled"]
        if len(pw) >= 8:
            args += ["wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", pw]
        rc, out, err = nm(*args)
        if rc == 0:
            rc, out, err = nm("connection", "up", "id", AP_CON, timeout=40)
        if rc != 0:
            self.ap_error = (err or out).strip().splitlines()[-1][:120] if (err or out).strip() else "AP failed"
            log("AP failed: " + self.ap_error)
            return False
        self.ap_error = ""
        self.ap_since = time.monotonic()
        # http://192.168.2.1 -> the app on 7000
        rule = ["PREROUTING", "-i", self.dev, "-p", "tcp", "--dport", "80", "-j", "REDIRECT", "--to-ports", "7000"]
        if sh("iptables", "-t", "nat", "-C", *rule)[0] != 0:
            sh("iptables", "-t", "nat", "-A", *rule)
        log("AP up: %s at %s" % (ssid, AP_IP))
        return True

    def ap_down(self):
        nm("connection", "down", "id", AP_CON, timeout=20)
        rule = ["PREROUTING", "-i", self.dev, "-p", "tcp", "--dport", "80", "-j", "REDIRECT", "--to-ports", "7000"]
        while sh("iptables", "-t", "nat", "-C", *rule)[0] == 0:
            if sh("iptables", "-t", "nat", "-D", *rule)[0] != 0:
                break
        self.ap_since = None

    def ap_has_clients(self):
        rc, out, _ = sh("ip", "neigh", "show", "dev", self.dev)
        return any(s in out for s in ("REACHABLE", "STALE", "DELAY", "PROBE"))

    def wait_other(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.other_network():
                return True
            time.sleep(2)
        return False

    # ── scan ────────────────────────────────────────────────────────────
    def refresh_scan(self):
        rc, out, _ = nm("-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                        "ifname", self.dev, "--rescan", "yes", timeout=25)
        if rc != 0:
            return False                        # with the AP up most radios cannot scan: keep the last list
        best = {}
        for line in out.splitlines():
            f = split_t(line)
            if len(f) < 3 or not f[0] or f[0] == self.ap_ssid():
                continue
            try:
                sig = int(f[1])
            except ValueError:
                continue
            rssi = sig // 2 - 100               # NetworkManager's 0..100 back to dBm, roughly
            sec = f[2].strip()
            n = {"ssid": f[0], "rssi": rssi, "bars": 3 if rssi >= -60 else 2 if rssi >= -72 else 1,
                 "secure": sec not in ("", "--"), "enterprise": "802.1X" in sec}
            if f[0] not in best or rssi > best[f[0]]["rssi"]:
                best[f[0]] = n
        self.scan = sorted(best.values(), key=lambda n: -n["rssi"])
        self.scan_t = time.time()
        return True

    # ── requests from the app ───────────────────────────────────────────
    def connect(self, ssid, password, identity):
        log("connect to %s%s" % (ssid, " (enterprise login)" if identity else ""))
        self.connecting = True
        self.error = ""
        self.write_state()
        had = nm("-t", "-f", "NAME", "connection", "show")[1].splitlines()
        if self.ap_on():
            self.ap_down()
            time.sleep(2)
        if identity:
            nm("connection", "delete", "id", ssid)
            rc, out, err = nm("connection", "add", "type", "wifi", "ifname", self.dev, "con-name", ssid,
                              "ssid", ssid, "wifi-sec.key-mgmt", "wpa-eap", "802-1x.eap", "peap",
                              "802-1x.phase2-auth", "mschapv2", "802-1x.identity", identity,
                              "802-1x.password", password)
            if rc == 0:
                rc, out, err = nm("connection", "up", "id", ssid, timeout=60)
        else:
            nm("device", "wifi", "rescan", "ifname", self.dev, timeout=15)
            time.sleep(3)
            args = ["--wait", "40", "device", "wifi", "connect", ssid, "ifname", self.dev]
            if password:
                args += ["password", password]
            rc, out, err = nm(*args, timeout=60)
        self.connecting = False
        if rc == 0 and self.wait_other(10):
            log("joined %s" % ssid)
            self.error = ""
            self.lost_at = None
            return
        msg = (err or out).lower()
        self.error = ("auth" if ("secrets" in msg or "password" in msg or "psk" in msg or "auth" in msg)
                      else "not_found" if ("no network with ssid" in msg or "not found" in msg)
                      else (err or out).strip()[:80] or "failed")
        log("could not join %s: %s" % (ssid, (err or out).strip()[:160]))
        if ssid not in had:
            nm("connection", "delete", "id", ssid)  # no half-made profile left behind
        if not self.wait_other(15):
            self.ap_up()

    def handle(self, req):
        op = req.get("op", "")
        if op == "scan":
            if not self.refresh_scan() and self.ap_on():
                pass                                # the list from before the AP came up
        elif op == "connect" and req.get("ssid"):
            self.connect(req["ssid"], req.get("password", ""), req.get("identity", ""))
        elif op == "disconnect":
            state, con = self.dev_state()
            if con and con != AP_CON:
                nm("device", "disconnect", self.dev)
            self.lost_at = time.monotonic() - LOST_GRACE_S      # the AP at once
        elif op == "forget":
            state, con = self.dev_state()
            if con and con != AP_CON:
                nm("connection", "delete", "id", con)
            self.lost_at = time.monotonic() - LOST_GRACE_S
        elif op == "ap-config":
            write_json(AP_CFG, {"ssid": req.get("ssid", ""), "password": req.get("password", "")})
            if self.ap_on():
                self.ap_down()
                self.ap_up()
        self.req_done = req.get("id", "")

    # ── state for the app and the screen ────────────────────────────────
    def write_state(self):
        state, con = self.dev_state()
        ap = con == AP_CON and state == "connected"
        sta = "connecting" if self.connecting else (
            "connected" if state == "connected" and not ap else
            "failed" if self.error else "disconnected")
        write_json(STATE, {
            "t": time.time(),
            "mode": "ap" if ap else "sta" if sta == "connected" else "none",
            "sta": {"state": sta, "ssid": self.sta_ssid() if sta == "connected" else "",
                    "ip": self.dev_ip() if sta == "connected" else "", "error": self.error},
            "ap": {"on": ap, "ssid": self.ap_ssid(), "ip": AP_IP, "error": self.ap_error},
            "scan": self.scan, "scan_t": self.scan_t, "req_done": self.req_done,
        })

    def write_host_net(self):
        rc, out, _ = sh("hostname", "-I")
        ips = [a for a in out.split() if a.count(".") == 3 and not docker_ip(a)]
        name = sh("hostname")[1].strip()
        tmp = HOST_NET + ".tmp"
        with open(tmp, "w") as f:
            f.write("%s %s\n" % (ips[0] if ips else "", name))
        if OWNER:
            try:
                os.chown(tmp, *OWNER)
            except OSError:
                pass
        os.replace(tmp, HOST_NET)

    # ── the loop ────────────────────────────────────────────────────────
    def run(self):
        while not self.dev:
            self.dev = self.find_dev()
            if not self.dev:
                log("no Wi-Fi device yet")
                time.sleep(5)
        log("Wi-Fi device %s, AP name %s" % (self.dev, self.ap_ssid()))
        last_state = 0.0
        while True:
            now = time.monotonic()
            try:
                if os.path.exists(REQ):
                    try:
                        with open(REQ) as f:
                            req = json.load(f)
                    except (OSError, ValueError):
                        req = {}
                    try:
                        os.remove(REQ)
                    except OSError:
                        pass
                    if req:
                        self.handle(req)
                        last_state = 0.0

                if now - last_state >= 2.0:
                    last_state = now
                    ap = self.ap_on()
                    if self.other_network():
                        self.lost_at = None
                        if ap:                      # a saved network is back: no AP needed
                            self.ap_down()
                    elif not ap and not self.connecting:
                        if self.lost_at is None:
                            self.lost_at = now
                        grace = BOOT_GRACE_S if now - self.t0 < BOOT_GRACE_S else LOST_GRACE_S
                        if now - self.lost_at >= grace and now - self.t0 >= min(grace, BOOT_GRACE_S):
                            self.ap_up()
                            self.lost_at = None
                    elif ap and self.ap_since and now - self.ap_since >= RETRY_EVERY_S \
                            and not self.ap_has_clients():
                        log("AP: nobody on it, trying the saved networks")
                        self.ap_down()
                        if not self.wait_other(RETRY_WAIT_S):
                            self.ap_up()
                    self.write_state()
                    self.write_host_net()
            except Exception as e:                  # keep running whatever nmcli does
                log("error: %r" % (e,))
                time.sleep(2)
            time.sleep(0.3)


if __name__ == "__main__":
    Wifi().run()
