# Building and distributing firmware

How to produce a `.bin` other people can flash, and how they flash it.

Nothing user-specific is compiled in. WiFi credentials, the address, ntfy settings and both
passwords live in NVS on the device and are entered through the setup portal on first boot.
Saved computers live in NVS too, and monitored sites on LittleFS. That is what makes the binary
shareable.

---

## 1. Prerequisites (you, the builder)

[arduino-cli](https://arduino.github.io/arduino-cli/) plus the ESP32 core (3.1.1 or newer;
built with 3.3.10) and two libraries:

```bash
arduino-cli core install esp32:esp32
```

```bash
arduino-cli lib install ArduinoJson ESPping
```

`WebServer`, `DNSServer`, `ESPmDNS`, `NetworkClientSecure`, `HTTPClient`, `LittleFS` and
`Preferences` all ship with the core.

## 2. Build

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=custom" --export-binaries MultiTool
```

That build sends the serial log to UART0, for a board reached through its UART port or a
USB-serial adapter (CP2102, CH343, FTDI on TX/RX). For one reached through its native USB port,
add `,CDCOnBoot=cdc` to the FQBN. Releases ship both; the native-USB files carry `-usbcdc`.

The artifacts land in `MultiTool/build/esp32.esp32.esp32s3/`:

| File | Size | Purpose |
| --- | --- | --- |
| `MultiTool.ino.bin` | 1.41 MB | the application (released as `…-app.bin`) |
| `MultiTool.ino.bootloader.bin` | 20 KB | second-stage bootloader |
| `MultiTool.ino.partitions.bin` | 3 KB | partition table (from `partitions.csv`) |
| `boot_app0.bin` | 8 KB | OTA selector |
| `MultiTool.ino.merged.bin` | **16 MB** | all of the above, pre-combined (released as `multitool-vX.Y.Z-esp32s3-n16r8.bin`) |

`PSRAM=opi` is not optional. Without it the blocklist, outbox and site list have nowhere to
live.

`PartitionScheme=custom` makes the build use the sketch's `partitions.csv`. Check that it did:

```bash
cat MultiTool/build/esp32.esp32.esp32s3/partitions.csv
```

It should show `spiffs` at `0x410000` with size `0xBE0000`. This is the same table as ESP32
AdBlocker, so an AdBlocker board can take MultiTool's app-only image and keep its data.

## 3. Confirm the binary carries no secrets

```bash
strings MultiTool/build/esp32.esp32.esp32s3/MultiTool.ino.bin | grep -iE "your-ssid|your-wifi-password|your-topic"
```

Substitute your own network name, password and topic. Expect **no output**. The core embeds a
couple of its own source paths, which include your macOS username. Those are not credentials.

Publish checksums alongside the binaries:

```bash
shasum -a 256 *.bin > SHA256SUMS.txt
```

---

## 4. Instructions for the person flashing

```bash
pip install esptool
```

| Connected through | macOS | Linux | Windows |
| --- | --- | --- | --- |
| native USB port | `/dev/cu.usbmodem*` | `/dev/ttyACM0` | `COMx` |
| UART port / USB-serial adapter | `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` | `/dev/ttyUSB0` | `COMx` |

Native USB needs no driver. A bridge chip may: [CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers),
[CH340/CH343](https://www.wch-ic.com/downloads/CH341SER_ZIP.html), FTDI (built into macOS).

Check the download first:

```bash
shasum -a 256 -c SHA256SUMS.txt --ignore-missing
```

### New board, or a clean start

Erase first, so nothing is inherited from whatever was on the board:

```bash
esptool --port /dev/cu.usbmodem101 erase-flash
```

Then write one file at `0x0`:

```bash
esptool --port /dev/cu.usbmodem101 --baud 921600 write-flash 0x0 multitool-v1.0.0-rc1-esp32s3-n16r8-usbcdc.bin
```

### Update (MultiTool, or a board running ESP32 AdBlocker)

Write only the application. Settings, the blocklist, your custom list, local DNS records, saved
computers and monitored sites all stay:

```bash
esptool --port /dev/cu.usbmodem101 --baud 921600 write-flash 0x10000 multitool-v1.0.0-rc1-esp32s3-n16r8-usbcdc-app.bin
```

A board that ran **WOL-NTFY or SiteCheck** has a different partition table: do the clean start
above, then re-add its computers or sites (see the README, *Coming from the separate
firmwares*).

### By the pieces

On the S3 the bootloader goes at `0x0`:

```bash
esptool --port /dev/cu.usbmodem101 --baud 921600 write-flash 0x0 MultiTool.ino.bootloader.bin 0x8000 MultiTool.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 MultiTool.ino.bin
```

The offsets are also in `flash_args` in the build folder. That file is authoritative if a
future core changes them.

Then follow **Quick start → Set it up** in the README.

## 5. Notes

- **The first boot after an erase formats LittleFS.** On a 12 MB partition this adds a few
  seconds before the device comes up.
- **To wipe only the settings over USB** (saved computers are in NVS too, so they go as well),
  erase the NVS partition:

  ```bash
  esptool --port /dev/cu.usbmodem101 erase-region 0x9000 0x5000
  ```

- **To wipe only the blocklist, custom list, local DNS records and monitored sites**, erase
  LittleFS:

  ```bash
  esptool --port /dev/cu.usbmodem101 erase-region 0x410000 0xBE0000
  ```
