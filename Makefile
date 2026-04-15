# Cydintosh helper Makefile
#
# This file exists to make common firmware/artifact flows explicit.
#
# IMPORTANT: Always use full-flash images when possible.
# The merged firmware image (without filesystem) pads up to 0x230000,
# which can overwrite the start of the disk/filesystem partition.
# Using full-flash images avoids this problem entirely.
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
# Flash memory layout (ESP32, 4MB):
#   0x001000  bootloader
#   0x008000  partition table
#   0x010000  application firmware
#   0x210000  patched Mac Plus ROM
#   0x230000  LittleFS filesystem (contains disk.img for the emulator)

SHELL := /bin/bash

WORKTREE_ORIGINAL := /workspace/tmp/cydintosh-upstream
UPSTREAM_BASE_COMMIT := cd5a6b2
PIO := /home/agent/.local/share/uv/tools/platformio/bin/pio
ESPTOOL := /home/agent/.local/share/uv/tools/esptool/bin/esptool
RETRO68_IMAGE := ghcr.io/autc04/retro68
SERIAL_PORT ?= /dev/cu.usbserial-210
BAUD ?= 460800

.PHONY: help prepare build firmware fs \
	full-flash-stable stable-artifacts flash-stable \
	original-worktree original-build original-artifacts full-flash-original flash-original \
	build-office-lights disk-update clean

help:
	@echo "Cydintosh build/flash targets"
	@echo ""
	@echo "  RECOMMENDED: use full-flash targets (single image, single command)"
	@echo ""
	@echo "Custom/fork targets:"
	@echo "  make prepare             - init submodules, generate Musashi ops, seed user_config.h and data/disk.img"
	@echo "  make build               - build firmware in current tree"
	@echo "  make firmware            - alias for make build"
	@echo "  make fs                  - generate LittleFS image (.pio/build/esp32dev/littlefs.bin)"
	@echo "  make stable-artifacts    - refresh all fork stable artifacts in web/"
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
	@echo ""
	@echo "Variables: SERIAL_PORT=$(SERIAL_PORT) BAUD=$(BAUD)"

# ---- Preparation ----

prepare:
	git submodule update --init --recursive
	ln -sf ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	make -C external/umac prepare
	@test -f include/user_config.h || cp include/user_config.h.tmpl include/user_config.h
	@mkdir -p data
	@test -f data/disk.img || cp cyd_800k.dsk data/disk.img

# ---- Build ----

build firmware:
	$(PIO) run

fs:
	$(PIO) run -t uploadfs --disable-auto-clean || true
	@test -f .pio/build/esp32dev/littlefs.bin
	@echo "LittleFS image: .pio/build/esp32dev/littlefs.bin"

# ---- Stable/fork artifacts ----

stable-artifacts: build fs
	@mkdir -p web web/mac-apps
	cp .pio/build/esp32dev/bootloader.bin web/bootloader-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/firmware.bin web/firmware-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/partitions.bin web/partitions-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/littlefs.bin web/littlefs-stable-mqtt-v1.bin
	$(ESPTOOL) --chip esp32 merge-bin -o web/full-flash-stable-mqtt-v1.bin \
	  --flash-mode dio --flash-freq 40m --flash-size 4MB \
	  0x1000 web/bootloader-stable-mqtt-v1.bin \
	  0x8000 web/partitions-stable-mqtt-v1.bin \
	  0x10000 web/firmware-stable-mqtt-v1.bin \
	  0x210000 rom_patched.bin \
	  0x230000 web/littlefs-stable-mqtt-v1.bin
	@test -f mac-app/OfficeLightsApp/build/OfficeLights.bin && \
	  cp mac-app/OfficeLightsApp/build/OfficeLights.bin web/mac-apps/OfficeLights-stable-mqtt-v1.bin || true
	@echo ""
	@echo "Stable artifacts refreshed under web/."
	@echo "Flash with: make flash-stable"

flash-stable:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) erase_flash
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/full-flash-stable-mqtt-v1.bin
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) verify_flash \
	  0x0000 web/full-flash-stable-mqtt-v1.bin

# ---- Original/upstream-equivalent artifacts ----

original-worktree:
	@if [ ! -d "$(WORKTREE_ORIGINAL)" ]; then \
	  git worktree add $(WORKTREE_ORIGINAL) $(UPSTREAM_BASE_COMMIT); \
	fi
	cd $(WORKTREE_ORIGINAL) && git submodule update --init --recursive
	cd $(WORKTREE_ORIGINAL) && ln -sf ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	cd $(WORKTREE_ORIGINAL) && make -C external/umac prepare
	cd $(WORKTREE_ORIGINAL) && cp include/user_config.h.tmpl include/user_config.h
	cd $(WORKTREE_ORIGINAL) && mkdir -p data && cp cyd_800k.dsk data/disk.img
	@test -f rom.bin && cp rom.bin $(WORKTREE_ORIGINAL)/rom.bin || true

original-build: original-worktree
	cd $(WORKTREE_ORIGINAL) && $(PIO) run
	cd $(WORKTREE_ORIGINAL) && $(PIO) run -t uploadfs --disable-auto-clean || true
	@test -f $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/littlefs.bin
	cd $(WORKTREE_ORIGINAL) && python3 tools/generate_patched_rom.py rom.bin -o rom_patched.bin
	cd $(WORKTREE_ORIGINAL) && mkdir -p web

original-artifacts: original-build
	@mkdir -p web
	$(ESPTOOL) --chip esp32 merge-bin -o web/full-flash-original.bin \
	  --flash-mode dio --flash-freq 40m --flash-size 4MB \
	  0x1000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/bootloader.bin \
	  0x8000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/partitions.bin \
	  0x10000 $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/firmware.bin \
	  0x210000 $(WORKTREE_ORIGINAL)/rom_patched.bin \
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

# ---- Cleanup ----

clean:
	rm -f web/full-flash-*.bin web/merged-firmware-*.bin web/littlefs-*.bin \
	  web/bootloader-*.bin web/firmware-*.bin web/partitions-*.bin \
	  web/mac-apps/*.bin 2>/dev/null || true
	@echo "Removed generated web artifacts."
