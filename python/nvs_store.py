"""nvs_store.py -- what the ESP kept in NVS, kept on Linux.

The sketch runs the ESP's robot.cpp, which saves its calibration ("offset1".."offset12",
float blobs), its config ("period", "height", "upHeight", "stride", "tilt", "sgspeed",
int32), the servo current caps ("cur1".."cur12"), the rest timer and a few markers in
NVS. On the UNO Q that store lives in RAM on the MCU (sketch/shim_esp.cpp); this file
is where it survives a power-off: nvs.json in the app folder. The sketch gets every key
at boot (mcu_link.py) and sends back each one it changes.
"""
import json
import os
import struct
import threading


class NvsStore:
    def __init__(self, base, log):
        self.path = os.path.join(base, "nvs.json")
        self.log = log
        self.lock = threading.Lock()
        self.data = {}            # key -> (type "i"|"b", raw bytes)
        self._timer = None
        try:
            with open(self.path) as f:
                for k, v in json.load(f).items():
                    self.data[k] = (v["t"], bytes.fromhex(v["hex"]))
        except FileNotFoundError:
            self._migrate(base)
        except (OSError, ValueError, KeyError) as e:
            self.log("nvs.json unreadable (%s): starting from the robot's defaults" % e)

    def _migrate(self, base):
        """first run of this version: keep the calibration saved by the previous one
        (settings.json "offsets_deg", degrees, the same convention as the ESP's offsets)"""
        try:
            with open(os.path.join(base, "settings.json")) as f:
                offs = json.load(f).get("offsets_deg")
        except (OSError, ValueError):
            offs = None
        if isinstance(offs, list) and len(offs) == 12 and any(float(x) != 0.0 for x in offs):
            for i, v in enumerate(offs):
                self.data["offset%d" % (i + 1)] = ("b", struct.pack("<f", float(v)))
            self.data["calseed"] = ("i", struct.pack("<i", 1))    # robot.cpp: do not re-seed over these
            self.log("calibration taken over from settings.json: %s" % ", ".join("%g" % float(x) for x in offs))
            self._save_now()

    def items(self):
        with self.lock:
            return list(self.data.items())

    def put(self, key, typ, raw):
        with self.lock:
            if typ == "x":                       # erased on the robot (nvs_erase_key)
                self.data.pop(key, None)
            else:
                self.data[key] = (typ, raw)
            # the robot can save a burst (twelve offsets): write once, a moment later
            if self._timer is None:
                self._timer = threading.Timer(0.5, self._save_now)
                self._timer.daemon = True
                self._timer.start()

    def offsets(self):
        out = []
        for i in range(1, 13):
            t = self.data.get("offset%d" % i)
            out.append(struct.unpack("<f", t[1])[0] if t and len(t[1]) == 4 else 0.0)
        return out

    def _save_now(self):
        with self.lock:
            self._timer = None
            doc = {k: {"t": t, "hex": raw.hex()} for k, (t, raw) in sorted(self.data.items())}
            tmp = self.path + ".tmp"
            try:
                with open(tmp, "w") as f:
                    json.dump(doc, f, indent=1)
                os.replace(tmp, self.path)
            except OSError as e:
                self.log("could not save nvs.json: %s" % e)
