"""mpx_unoq -- Mini Pupper on the Arduino UNO Q, behaving like the ESP32-S3 firmware
(mpv2onlyRL): the same movement code on the MCU and the same web app here.

Open http://<board>:7000 for the ESP's web app (pwa-claw, in assets/). The routes
it calls are answered by esp_api.py; the robot and Servo Studio ones by the sketch.

Files in the app folder:
  nvs.json        what the ESP kept in NVS: calibration, config, current caps
  settings.json   this app's own settings
  display/        the GIFs you added on the Display page (the built-in ones: python/display_gifs)
"""
import collections
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from arduino.app_utils import App                      # noqa: E402
from arduino.app_bricks.web_ui import WebUI            # noqa: E402

import display_api                                     # noqa: E402
import esp_api                                         # noqa: E402
from mcu_link import McuLink                           # noqa: E402
from nvs_store import NvsStore                         # noqa: E402
from policy_net import Policies                        # noqa: E402

BASE = os.path.dirname(HERE)


class Log:
    def __init__(self):
        self.lines = collections.deque(maxlen=300)

    def __call__(self, msg):
        line = time.strftime("%H:%M:%S ") + msg
        self.lines.append(line)
        print(line, flush=True)


class Settings(dict):
    def __init__(self, path):
        super().__init__()
        self.path = path
        try:
            with open(path) as f:
                self.update(json.load(f))
        except (OSError, ValueError):
            pass

    def save(self):
        try:
            with open(self.path, "w") as f:
                json.dump(dict(self), f, indent=2)
        except OSError:
            pass


log = Log()
settings = Settings(os.path.join(BASE, "settings.json"))
nvs = NvsStore(BASE, log)
link = McuLink(nvs, log)
policies = Policies(log)       # the RL networks the sketch asks for (pol_fwd)
ui = WebUI()
esp_api.register(ui.app, link, settings, log,
                 extra=[lambda app: display_api.register(app, link, log, BASE)])
log("web app on port 7000; waiting for the sketch")
App.run()
