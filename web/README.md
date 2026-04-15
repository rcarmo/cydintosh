# Cydintosh web flasher artifacts

This directory contains the browser flasher page and stable firmware artifacts.

## Browser flasher

- `index.html`
- `manifest.json`
- `server.py`

## Recommended: full-flash images

Always use the full-flash images when flashing. These include bootloader,
partition table, firmware, patched ROM, and LittleFS filesystem in one file.

| Image | Description |
| --- | --- |
| `full-flash-original.bin` | Upstream-equivalent vanilla build |
| `full-flash-stable-mqtt-v1.bin` | Fork build with MQTT smart-home layer |

### Flash command

```bash
esptool --port <serial-port> --baud 115200 write_flash \
  0x0000 web/full-flash-original.bin
```

or:

```bash
esptool --port <serial-port> --baud 115200 write_flash \
  0x0000 web/full-flash-stable-mqtt-v1.bin
```

## Flash memory layout

| Offset     | Content                  |
| ---------- | ------------------------ |
| `0x001000` | Bootloader               |
| `0x008000` | Partition table          |
| `0x010000` | Application firmware     |
| `0x210000` | Patched Mac Plus ROM     |
| `0x230000` | LittleFS (disk.img)      |

**WARNING:** The older merged firmware images (without filesystem) pad up to
`0x230000`, which overwrites the start of the filesystem partition. Always use
`full-flash-*.bin` images instead.

## Individual artifacts (for reference / advanced use only)

### Stable MQTT build
- `bootloader-stable-mqtt-v1.bin`
- `partitions-stable-mqtt-v1.bin`
- `firmware-stable-mqtt-v1.bin`
- `littlefs-stable-mqtt-v1.bin`
- `merged-firmware-stable-mqtt-v1.bin` — **do not flash alone**

### Original/upstream-equivalent
- `littlefs-original.bin`
- `cyd_800k-original.dsk` — pristine HFS disk image (reference only, not flashable)

### Mac app artifacts
- `mac-apps/OfficeLights-stable-mqtt-v1.bin` — MacBinary build artifact (reference)
