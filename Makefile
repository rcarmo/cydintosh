# Cydintosh helper Makefile
#
# This file exists to make common firmware/artifact flows explicit.
#
# IMPORTANT: Always use board-specific full-flash images when possible.
# They include the bootloader at the correct chip-specific offset:
# ESP32 at 0x1000, ESP32-S3 at 0x0. Firmware-only/partial images are
# easy to flash at the wrong offset and do not include ROM/disk contents.
#
# Stable/custom artifacts live under web/ and are intended for this fork.
# Original artifacts are generated from the last pre-fork commit and copied into web/.
#
# Requirements (host):
# - bash / python3
# - build-essential (for Musashi code generation)
# - uv + PlatformIO (or equivalent pio on PATH)
# - esptool (for merge/flash helper flows)
# - docker (optional, for Retro68 Mac app builds and HFS disk image updates)
#
# Flash memory layout varies by board profile:
#   esp32-cyd2usb:     4MB,  ROM at 0x210000, LittleFS at 0x230000
#   esp32-8048s043c:  16MB,  ROM at 0x410000, LittleFS at 0x430000

SHELL := /bin/bash

WORKTREE_ORIGINAL := /workspace/tmp/cydintosh-upstream
UPSTREAM_BASE_COMMIT := cd5a6b2
PIO ?= pio
ESPTOOL ?= /workspace/.venvs/pio/bin/python -m esptool
SERIAL_PYTHON ?= /workspace/.venvs/pio/bin/python
RETRO68_IMAGE := ghcr.io/autc04/retro68
UMAC_PATCHES := patches/umac-suppress-sony-eject.patch \
	patches/umac-esp32-hotpath-tuning.patch
PIO_ENV ?= esp32-cyd2usb
BUILD_DIR := .pio/build/$(PIO_ENV)
ARTIFACT_SUFFIX ?= $(PIO_ENV)
ifeq ($(PIO_ENV),esp32-8048s043c)
ROM_IMAGE ?= .pio/roms/rom_patched-480x800.bin
ROM_WIDTH ?= 480
ROM_HEIGHT ?= 800
else ifeq ($(PIO_ENV),esp32-cyd2usb-mac512x384-rotfit)
ROM_IMAGE ?= .pio/roms/rom_patched-512x384.bin
ROM_WIDTH ?= 512
ROM_HEIGHT ?= 384
else
ROM_IMAGE ?= .pio/roms/rom_patched.bin
ROM_WIDTH ?= 240
ROM_HEIGHT ?= 320
endif
SERIAL_PORT ?= /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
BAUD ?= 460800
TAB5_LC_SERIAL_PORT ?= /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D1:46:0D-if00
TAB5_LC_BAUD ?= 921600
TAB5_LC_ROM_IMAGE ?= vendor/mac-lc.rom
TAB5_LC_ROM_OFFSET ?= 0x410000
TAB5_LC_DISK_IMAGE ?= vendor/lc-disk.img
TAB5_LC_CAPTURE_DURATION ?= 30

ifeq ($(PIO_ENV),esp32-8048s043c)
ESP_CHIP ?= esp32s3
FLASH_SIZE ?= 16MB
FLASH_FREQ ?= 80m
BOOTLOADER_OFFSET ?= 0x0
ROM_OFFSET ?= 0x410000
DISK_OFFSET ?= 0x430000
else
ESP_CHIP ?= esp32
FLASH_SIZE ?= 4MB
FLASH_FREQ ?= 40m
BOOTLOADER_OFFSET ?= 0x1000
ROM_OFFSET ?= 0x210000
DISK_OFFSET ?= 0x230000
endif

.PHONY: help prepare build firmware fs \
	build-cyd2usb build-8048s043c build-tab5-lc flash-tab5-lc build-tab5-display-smoke flash-tab5-display-smoke build-tab5-bootdiag flash-tab5-bootdiag flash-tab5-lc-rom capture-tab5-logs lc-rom-info lc-rom-vectors lc-rom-io-hints lc-disk-info lc-video-test-pattern stable-artifacts flash-stable \
	original-worktree original-build original-artifacts flash-original \
	build-office-lights disk-update capture-logs prepare-rom prepare-disk clean

help:
	@echo "Cydintosh build/flash targets"
	@echo ""
	@echo "Reference flash commands:"
	@echo "  make stable-artifacts PIO_ENV=$(PIO_ENV)"
	@echo "  make flash-stable PIO_ENV=$(PIO_ENV) SERIAL_PORT=$(SERIAL_PORT)"
	@echo ""
	@echo "  RECOMMENDED: use full-flash targets (single image, single command)"
	@echo ""
	@echo "Custom/fork targets:"
	@echo "  make prepare             - init submodules, generate Musashi ops, seed user_config.h and data/disk.img"
	@echo "  make build               - build firmware in current tree for PIO_ENV=$(PIO_ENV)"
	@echo "  make build-cyd2usb       - build existing ESP32 CYD2USB profile"
	@echo "  make build-8048s043c     - build ESP32-8048S043C/S3 profile"
	@echo "  make build-tab5-lc       - build ESP32-P4 M5Stack Tab5 LC/color skeleton"
	@echo "  make flash-tab5-lc       - flash ESP32-P4 M5Stack Tab5 LC/color skeleton [explicit target]"
	@echo "  make build-tab5-display-smoke - build M5Stack BSP-based Tab5 DSI display smoke image"
	@echo "  make flash-tab5-display-smoke - flash M5Stack BSP-based Tab5 DSI display smoke image [explicit target]"
	@echo "  make build-tab5-bootdiag - build minimal Tab5 GPIO22/USB boot diagnostic image"
	@echo "  make flash-tab5-bootdiag - flash minimal Tab5 GPIO22/USB boot diagnostic image [explicit target]"
	@echo "  make flash-tab5-lc-rom   - validate and flash local vendor/mac-lc.rom to Tab5 LC ROM partition"
	@echo "  make capture-tab5-logs   - capture ESP32-P4 Tab5 serial logs"
	@echo "  make lc-rom-info         - inspect local vendor/mac-lc.rom metadata only"
	@echo "  make lc-rom-vectors      - scan local ROM metadata for plausible reset-vector/window candidates"
	@echo "  make lc-rom-io-hints     - scan local ROM metadata for 0x50fxxxxx / 24-bit I/O constants"
	@echo "  make lc-disk-info        - inspect local vendor/lc-disk.img metadata if present"
	@echo "  make lc-video-test-pattern - render LC indexed debug pattern to artifacts/lc-video-test-pattern.ppm"
	@echo "  make firmware            - alias for make build"
	@echo "  make fs                  - generate LittleFS image ($(BUILD_DIR)/littlefs.bin)"
	@echo "  make stable-artifacts    - refresh fork artifacts in web/ for PIO_ENV=$(PIO_ENV)"
	@echo "  make flash-stable        - flash full stable image (firmware + ROM + filesystem) [RECOMMENDED]"
	@echo ""
	@echo "Original/upstream-equivalent targets:"
	@echo "  make original-worktree   - create/update clean upstream-equivalent worktree"
	@echo "  make original-build      - build firmware + filesystem in clean upstream-equivalent worktree"
	@echo "  make original-artifacts  - copy original artifacts into web/"
	@echo "  make flash-original      - flash full original image (firmware + ROM + filesystem) [RECOMMENDED]"
	@echo ""
	@echo "Mac app / disk targets:"
	@echo "  make build-office-lights - build OfficeLights Mac app via Retro68 Docker image"
	@echo "  make disk-update         - update data/disk.img with built Mac apps using Retro68 container HFS tools"
	@echo "  make capture-logs        - reset board and capture 10s of serial logs to logs/*.log"
	@echo ""
	@echo "Variables: PIO_ENV=$(PIO_ENV) SERIAL_PORT=$(SERIAL_PORT) BAUD=$(BAUD)"

# ---- Preparation ----

prepare:
	git submodule update --init --recursive
	@for patch in $(UMAC_PATCHES); do \
	  if git -C external/umac apply --check ../../$$patch 2>/dev/null; then \
	    echo "Applying $$patch"; \
	    git -C external/umac apply ../../$$patch; \
	  elif git -C external/umac apply --reverse --check ../../$$patch 2>/dev/null; then \
	    echo "$$patch already applied"; \
	  else \
	    echo "ERROR: cannot apply $$patch" >&2; \
	    exit 1; \
	  fi; \
	done
	ln -sfn ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	make -C external/umac prepare
	@test -f include/user_config.h || cp include/user_config.h.tmpl include/user_config.h
	@mkdir -p data
	@test -f data/disk.img || ( test -f vendor/disk.img && cp vendor/disk.img data/disk.img || echo 'Place a bootable 800K HFS disk image at vendor/disk.img or data/disk.img' )

# ---- Build ----

build firmware: prepare
	$(PIO) run -e $(PIO_ENV)

build-cyd2usb:
	$(MAKE) build PIO_ENV=esp32-cyd2usb

build-8048s043c:
	$(MAKE) build PIO_ENV=esp32-8048s043c

build-tab5-lc:
	$(PIO) run -e esp32-p4-tab5-lc-color

flash-tab5-lc:
	$(PIO) run -e esp32-p4-tab5-lc-color -t upload

build-tab5-display-smoke:
	$(PIO) run -e esp32-p4-tab5-display-smoke

flash-tab5-display-smoke:
	$(PIO) run -e esp32-p4-tab5-display-smoke -t upload

build-tab5-bootdiag:
	$(PIO) run -e esp32-p4-tab5-bootdiag

flash-tab5-bootdiag:
	$(PIO) run -e esp32-p4-tab5-bootdiag -t upload

flash-tab5-lc-rom: lc-rom-info
	$(ESPTOOL) --chip esp32p4 --port $(TAB5_LC_SERIAL_PORT) --baud $(TAB5_LC_BAUD) write_flash $(TAB5_LC_ROM_OFFSET) $(TAB5_LC_ROM_IMAGE)
	$(ESPTOOL) --chip esp32p4 --port $(TAB5_LC_SERIAL_PORT) --baud $(TAB5_LC_BAUD) verify_flash $(TAB5_LC_ROM_OFFSET) $(TAB5_LC_ROM_IMAGE)

capture-tab5-logs:
	$(SERIAL_PYTHON) tools/capture_serial_logs.py --port $(TAB5_LC_SERIAL_PORT) --baud 115200 --duration $(TAB5_LC_CAPTURE_DURATION) --dtr-during-reset false --no-clear-after-reset

lc-rom-info:
	python3 tools/inspect_lc_rom.py $(TAB5_LC_ROM_IMAGE)

lc-rom-vectors:
	python3 tools/inspect_lc_rom.py $(TAB5_LC_ROM_IMAGE) --vector-scan --entry-scan

lc-rom-io-hints:
	python3 tools/inspect_lc_rom.py $(TAB5_LC_ROM_IMAGE) --io-scan

lc-disk-info:
	python3 tools/inspect_lc_disk.py $(TAB5_LC_DISK_IMAGE) --allow-missing

lc-video-test-pattern:
	python3 tools/render_lc_video_pattern.py artifacts/lc-video-test-pattern.ppm

fs:
	$(PIO) run -e $(PIO_ENV) -t buildfs
	@test -f $(BUILD_DIR)/littlefs.bin
	@echo "LittleFS image: $(BUILD_DIR)/littlefs.bin"

# ---- Stable/fork artifacts ----

stable-artifacts: build fs
	@mkdir -p web web/mac-apps
	cp $(BUILD_DIR)/bootloader.bin web/bootloader-$(ARTIFACT_SUFFIX).bin
	cp $(BUILD_DIR)/firmware.bin web/firmware-$(ARTIFACT_SUFFIX).bin
	cp $(BUILD_DIR)/partitions.bin web/partitions-$(ARTIFACT_SUFFIX).bin
	cp $(BUILD_DIR)/littlefs.bin web/littlefs-$(ARTIFACT_SUFFIX).bin
	@test -f $(ROM_IMAGE) || $(MAKE) prepare-rom PIO_ENV=$(PIO_ENV) ROM_IMAGE=$(ROM_IMAGE) ROM_WIDTH=$(ROM_WIDTH) ROM_HEIGHT=$(ROM_HEIGHT)
	$(ESPTOOL) --chip $(ESP_CHIP) merge-bin -o web/full-flash-$(ARTIFACT_SUFFIX).bin \
	  --flash-mode dio --flash-freq $(FLASH_FREQ) --flash-size $(FLASH_SIZE) \
	  $(BOOTLOADER_OFFSET) web/bootloader-$(ARTIFACT_SUFFIX).bin \
	  0x8000 web/partitions-$(ARTIFACT_SUFFIX).bin \
	  0x10000 web/firmware-$(ARTIFACT_SUFFIX).bin \
	  $(ROM_OFFSET) $(ROM_IMAGE) \
	  $(DISK_OFFSET) web/littlefs-$(ARTIFACT_SUFFIX).bin
	@test -f mac-app/OfficeLightsApp/build/OfficeLights.bin && \
	  cp mac-app/OfficeLightsApp/build/OfficeLights.bin web/mac-apps/OfficeLights-$(ARTIFACT_SUFFIX).bin || true
	@echo ""
	@echo "Stable artifacts refreshed under web/ for $(PIO_ENV)."
	@echo "Flash with: make flash-stable PIO_ENV=$(PIO_ENV) SERIAL_PORT=..."

flash-stable:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) erase_flash
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) --after no-reset write_flash \
	  0x0000 web/full-flash-$(ARTIFACT_SUFFIX).bin
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) verify_flash \
	  0x0000 web/full-flash-$(ARTIFACT_SUFFIX).bin

# ---- Original/upstream-equivalent artifacts ----

original-worktree:
	@if [ ! -d "$(WORKTREE_ORIGINAL)" ]; then \
	  git worktree add $(WORKTREE_ORIGINAL) $(UPSTREAM_BASE_COMMIT); \
	fi
	cd $(WORKTREE_ORIGINAL) && git submodule update --init --recursive
	cd $(WORKTREE_ORIGINAL) && ln -sf ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	cd $(WORKTREE_ORIGINAL) && make -C external/umac prepare
	cd $(WORKTREE_ORIGINAL) && cp include/user_config.h.tmpl include/user_config.h
	cd $(WORKTREE_ORIGINAL) && mkdir -p data && test -f vendor/disk.img && cp vendor/disk.img data/disk.img || echo "No disk image"
	@test -f vendor/rom.bin && cp vendor/rom.bin $(WORKTREE_ORIGINAL)/rom.bin || true

original-build: original-worktree
	cd $(WORKTREE_ORIGINAL) && $(PIO) run
	cd $(WORKTREE_ORIGINAL) && $(PIO) run -t uploadfs --disable-auto-clean || true
	@test -f $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/littlefs.bin
	cd $(WORKTREE_ORIGINAL) && mkdir -p data && python3 tools/generate_patched_rom.py rom.bin -o data/rom_patched.bin
	cd $(WORKTREE_ORIGINAL) && mkdir -p web

original-artifacts: original-build
	@mkdir -p web
	$(ESPTOOL) --chip esp32 merge-bin -o web/full-flash-original.bin \
	  --flash-mode dio --flash-freq 40m --flash-size 4MB \
	  0x1000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/bootloader.bin \
	  0x8000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/partitions.bin \
	  0x10000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/firmware.bin \
	  0x210000 $(WORKTREE_ORIGINAL)/data/rom_patched.bin \
	  0x230000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/littlefs.bin
	cp $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/littlefs.bin web/littlefs-original.bin
	@echo ""
	@echo "Original artifacts refreshed under web/."
	@echo "Flash with: make flash-original"

flash-original:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) erase_flash
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/full-flash-original.bin
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) verify_flash \
	  0x0000 web/full-flash-original.bin

# ---- Mac app builds ----

build-office-lights:
	sudo docker run --rm --user $$(id -u):$$(id -g) -v $(CURDIR)/mac-app:/root -i $(RETRO68_IMAGE) /bin/bash -lc '\
	set -e; \
	cd /root/OfficeLightsApp; \
	rm -rf build; mkdir build; cd build; \
	cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake; \
	make -j2'
	python3 tools/set_macbinary_flags.py mac-app/OfficeLightsApp/build/OfficeLights.bin +bndl

# ---- HFS disk image update ----

disk-update:
	sudo docker run --rm -v $(CURDIR):/work -w /work $(RETRO68_IMAGE) /bin/bash -lc '\
	set -e; \
	/Retro68-build/toolchain/bin/hmount data/disk.img; \
	/Retro68-build/toolchain/bin/hdel OfficeLights || true; \
	/Retro68-build/toolchain/bin/hcopy -m mac-app/OfficeLightsApp/build/OfficeLights.bin :OfficeLights; \
	/Retro68-build/toolchain/bin/hdir; \
	/Retro68-build/toolchain/bin/humount'

capture-logs:
	python3 tools/capture_serial_logs.py --port $(SERIAL_PORT) --baud 115200 --duration 10

# ---- Cleanup ----

clean:
	rm -f web/full-flash-*.bin web/merged-firmware-*.bin web/littlefs-*.bin \
	  web/bootloader-*.bin web/firmware-*.bin web/partitions-*.bin \
	  web/mac-apps/*.bin 2>/dev/null || true
	@echo "Removed generated web artifacts."

# ---- Vendor asset preparation ----

prepare-rom:
	@test -f vendor/rom.bin || ( echo "ERROR: Place Mac Plus ROM v3 (4D1F8172, 128KB) at vendor/rom.bin" && exit 1 )
	@mkdir -p $(dir $(ROM_IMAGE))
	python3 tools/generate_patched_rom.py vendor/rom.bin --width $(ROM_WIDTH) --height $(ROM_HEIGHT) -o $(ROM_IMAGE)
	@echo "Patched ROM written to $(ROM_IMAGE)"

prepare-disk:
	@test -f vendor/disk.img && echo "vendor/disk.img exists" || echo "NOTE: No vendor/disk.img. See README for disk image preparation."
	@mkdir -p data
	@test -f data/disk.img || ( test -f vendor/disk.img && cp vendor/disk.img data/disk.img && echo "Copied vendor/disk.img → data/disk.img" || echo "Place a bootable 800K HFS disk image at vendor/disk.img" )
