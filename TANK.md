# arduino_tank fork of the UNO R4 WiFi bridge

- Official 0.6.1 sources on upstream arduino-esp32 **2.0.17** (ESP-IDF 4.4.7) instead of 2.0.9 (4.4.4).
  IDF 4.4.4 never cancels the association-comeback timer, so with a PMF access point the station
  re-associated every 268.435 s and lost ~9.5 s each time (esp-idf commit 0c8ec2a0, in 4.4.5+).
  Arduino's core patches ported: boards.txt entry, `tools/partitions/unor4wifi.csv`, gnu++17, USBHID string.
- `UNOR4USBBridge/cmds_tank.h`: opt-in AT extension (`AT+TANKVER?`, `AT+TANKLINK=<0|1>`, `AT+TANKBAUD=<baud>`),
  described in the file header. Stock sketches never send these and see stock behaviour.
- Other changes: a 1 s timeout on the CLIENTSEND payload read, CLIENTRECEIVE no longer pads short reads,
  modem sleep back on before BLE (HCI begin).
- The core submodule `hardware/esp32-patched/esp32` points at espressif/arduino-esp32 tag 2.0.17
  (5e19e08); Arduino's patches for it are in `tank/core-2.0.17-arduino-patches.diff`:
  `git submodule update --init --recursive`, then
  `git -C hardware/esp32-patched/esp32 apply ../../../tank/core-2.0.17-arduino-patches.diff`
  and the core's `tools/get.py` (toolchains) as for the stock build.
- Build: `sed "s#PWD#$PWD#g" arduino-cli.yaml.orig > arduino-cli.yaml` then line 2 of `compile.sh`.
  Image: the official `UNOR4-WIFI-S3-0.6.1.bin` with only app0 (0x50000) replaced by `UNOR4USBBridge.ino.bin`.
- Flash: `unor4wifi-reboot-macos`, then `espflash write-bin -p <port> -b 115200 0x0 <image>`, then a full
  power cycle (USB and battery). Rollback: the official `UNOR4-WIFI-S3-0.6.1.bin` the same way.
- `ESP_FW_VERSION` stays "0.6.1" so WiFiS3 is happy; the extension is detected with `AT+TANKVER?`.
