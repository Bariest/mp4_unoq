#!/bin/bash -l
# Start the robot app at power-on (crontab: @reboot, README step 4).
# The sketch reaches Linux by itself after a power-on; the flash at the end is
# only for the case where Linux restarted while the MCU kept running.
APP=/home/arduino/ArduinoApps/mpx_unoq
# (the address under the eyes and the Wi-Fi: tools/mpx_wifi.py, the mpx-wifi service)
running() { arduino-app-cli app ps 2>/dev/null | grep -q "mpx_unoq running"; }
ok()      { curl -s -m 3 http://127.0.0.1:7000/unoq/hello | grep -q '"fw"'; }
# start the app as soon as Linux can run it (retry every 2 s, up to 1 min)
for i in $(seq 1 30); do running && break; arduino-app-cli app start $APP; sleep 2; done
# wait for the app to reach the MCU (check every 2 s, up to 1 min)
for i in $(seq 1 30); do ok && echo "ready after start" && exit 0; sleep 2; done
# still nothing: reset the MCU (flash the cached build) and start the app again
cd /home/arduino/mpx_mcu/sketch && arduino-cli compile --upload .
arduino-app-cli app stop $APP
arduino-app-cli app start $APP
