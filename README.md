# Mini Pupper on the Arduino UNO Q, behaving like firmware-dev

This app makes the UNO Q (on its carrier board, `Arduino_UNO_Q_EXT`) run the
**ESP32-S3 firmware's own code** (firmware-dev) and serve **its own web app**.

| | runs | comes from firmware-dev |
|---|---|---|
| MCU (STM32U585, `sketch/`) | every gait and move, 200 Hz servo frames, power / rest, look, joystick, calibration, Servo Studio, the boot check of the servo boards, the battery voltage, the IMU (BMI270 + BMM150), the LCD face with its emotions and GIF animations, the `/v1/robot` + `/v1/studio` HTTP handlers | `main/robot/*`, `main/display/*`, `main/network/http_server.cc` (robot routes), Bosch's BMI270 / BMM150 drivers, LVGL 9.5 (copied by `tools/port_from_esp.py` and `tools/vendor_lvgl.py`) |
| MCU, RL | the RL runner: 200 Hz ticks, observations, history, decoder, every guard and stop rule, get-up, keep up, stairs, the `/v1/policy` handlers | `main/rl/policy_run.cc`, `policy_core.c`, `main/network/http_policy.cc` |
| Linux (`python/`) | the web server, the saved settings (`nvs.json`), the TrajOpt / Character move rows, the GIF files of the Display page, **the RL networks** (`policy_net.py`, numpy), Wi-Fi and system pages | `www/` (served unchanged from `assets/`), `main/rl/policies/*.mpxp` |

## Install / update

1. **Once: the Bridge at 460800 baud.** The face's GIFs and the RL policy need
   more than the router's default 115200. On the board:

       sudo systemctl edit arduino-router

   and put in the editor that opens:

       [Service]
       ExecStart=
       ExecStart=/usr/bin/arduino-router --unix-port /var/run/arduino-router.sock --serial-port /dev/ttyHS1 --serial-baudrate 460800

   then `sudo systemctl restart arduino-router`. (Without this the screen stays on
   "Linux is starting" and after 100 s says the router is not at 460800.)
   Check the line first with `systemctl cat arduino-router`: keep its own options
   and only add `--serial-baudrate 460800`.
2. **The MCU sketch is flashed with arduino-cli, not App Lab.** App Lab builds
   sketches in the core's "dynamic" mode, where the code is loaded into the
   MCU's 256 KB sketch RAM; this sketch (~400 KB of code: robot, LVGL, RL) does
   not fit there. In "static" mode it runs from the 768 KB flash partition.
   The app folder therefore has no `sketch/`: App Lab runs only the Python side
   and leaves the MCU alone. On the board:

       mkdir -p ~/mpx_mcu
       # copy the PC's mpx_unoq\sketch folder to ~/mpx_mcu/sketch (scp -r ... arduino@<IP>:/home/arduino/mpx_mcu/)
       cd ~/mpx_mcu/sketch && arduino-cli compile --upload .

   (`sketch.yaml` picks `link_mode=static,wait_linux_boot=no`: from flash, and
   starting at power-on.) Run this again whenever `sketch/` changes.
3. The Linux side: copy `python`, `assets`, `tools`, `app.yaml` to
   `~/ArduinoApps/mpx_unoq/` (and no `sketch` folder there), then
   `arduino-app-cli app restart ~/ArduinoApps/mpx_unoq`.
4. Make it start by itself at power-on (the start script is in `tools/`):

       cp ~/ArduinoApps/mpx_unoq/tools/mpx_start.sh ~/ && chmod +x ~/mpx_start.sh
       (crontab -l 2>/dev/null | grep -v mpx_start; echo "@reboot /home/arduino/mpx_start.sh > /home/arduino/app_autostart.log 2>&1") | crontab -

   It starts the app, waits until the app reaches the MCU, and only if it does
   not within a minute flashes the MCU again (`~/app_autostart.log` ends with
   "ready after start" when all went well). The face is up ~1 min after power-on.
5. Calibration and settings live on the board, in `~/ArduinoApps/mpx_unoq/nvs.json`.
   Keep a copy on the PC (`scp arduino@<IP>:/home/arduino/ArduinoApps/mpx_unoq/nvs.json .`)
   and copy it back to a new board before the first start.

## Power-on: the screen and the web app

The sketch is in the MCU's flash, so the screen comes up at once, before Linux:
"Mini Pupper / Linux is starting" with the seconds counting. Then:

1. "Linux is up - waiting for the robot app" (the router answered),
2. "Linux is up: 192.168.1.113:7000 ..." (the app's Python runs: the web app is
   being served from now on),
3. "Loading the saved settings" -> "Starting the servos" -> the eyes, with the
   address under them.

So the address under the eyes is the moment the web app is ready. The UNO Q's
Linux and App Lab's Docker take about a minute after the Arduino logo; that part
is the board's, the app cannot start earlier, but with step 4 it starts as soon
as App Lab can.

## Carrier board

| | pins |
|---|---|
| Servo bus (SPI2) | D13 SCK, D11 MOSI, D12 MISO, **4 MHz** (`sketch/shim_esp.h` CONFIG_APP_SERVO_SPI_KHZ; the ESP runs 12) |
| Servo connectors | **CN2 = FL** (D10), **CN3 = FR** (D9), **CN5 = RR** (D7), **CN4 = RL** (D8) |
| LCD (ST7789, J1) | SPI1: A1 SCK, A3 MOSI, A0 CS; D5 DC, D4 RESET, D6 backlight (P-FET, low = on), 40 MHz |
| IMU | BMI270 + BMM150 on I2C2: D20 SDA, D21 SCL (address 0x68, or 0x69 if only that answers) |

Plug the legs into the connectors as in the table (the same legs as the wiring
before). "Current": the servo boards take their power straight from VBat on the
connectors now, so a weak supply no longer limits them through thin wires - there
is nothing to set in software for it.

At boot the ESP's boot check runs (each board's replies, the SPI line): a problem
shows on the screen for 30 s and in `/unoq/log`.

## Display page

As on the ESP: brightness, flip / mirror / shift, colours, emotions, text, and the
GIFs (blink, smile, sleepy, and your own up to 200 KB, 320 x 240). The GIF files
stay on Linux (`python/display_gifs/`, your uploads in `<app>/display/`); the
chosen one is sent into the MCU's RAM (a few seconds) and played from there.

## RL

The three policies firmware-dev builds in are here: the walk (`default.mpxp`,
"walk"), the SKU1 v2 recovery walker (`sku1v2_recovery`, the RL page's
Balance + get up / keep up) and its get-up (`getup_sku1`). The RL page works as
on the ESP.

The networks themselves (0.13 - 0.77 MB of weights each) do not fit the MCU: it
keeps the files without their weights (`sketch/policy_files.c`) and, at every
20 ms policy step, sends the observation to Linux, which evaluates the network
(`python/policy_net.py`, numpy, the full files in `python/policies/`) and answers
the 12 actions. Everything with a deadline stays on the MCU: the 5 ms ticks, the
servo exchange, the IMU, the tilt / current / temperature guards and the stops.
The RL page's "network" time is that round trip (a few ms; the step allows 20).
The recovery walker's 10-step history is kept on both sides; only the newest
observation goes over the Bridge.

- Needs the 460800 baud of Install step 1, and numpy (`python/requirements.txt`:
  App Lab installs it when the app starts the first time - the board needs
  internet then).
- Weights are always the file's floats (no 8 / 16-bit: Linux has the room). The
  size test (bench) measures the MCU, which has no room for big networks.
- The run and get-up logs cannot be saved as files (no file system on the MCU);
  the RL page shows them from memory. "Run without Wi-Fi" changes nothing here.
- If Linux does not answer a step, the runner stops the run as on the ESP for a
  bad network output (graceful stop, or limp in a get-up).

## Where things are kept

- `nvs.json` (app folder): what the ESP kept in NVS - calibration, config, current
  caps, rest timer, the display settings (`display.*`), controller scheme.
- `python/tj_tables.bin`: the TrajOpt / Character tables, streamed to the MCU while
  a move plays.

## Diagnostics

- `http://<board>:7000/unoq/hello` - the sketch: version, boards, SPI, IMU, LCD, GIF, free RAM.
- `http://<board>:7000/unoq/log` - the robot's log lines (the ESP's serial log).
- `http://<board>:7000/unoq/spi?edge=1&khz=4000` - servo bus clock / edge, live.

## Updating from firmware-dev

On the PC: `python tools/port_from_esp.py C:\esp\projects\firmware-dev` (robot,
display, RL, routes, Bosch drivers from its `managed_components`, cJSON kept as it
is in `sketch/`, the policies into `python/policies/`), and when LVGL
changes `python tools/vendor_lvgl.py C:\esp\projects\firmware-dev\lvgl`
(needs gcc). Copy `www/*` into `assets/`.
