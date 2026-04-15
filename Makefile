# Cydintosh helper Makefile
#
# This file exists to make common firmware/artifact flows explicit.
#
# Stable/custom artifacts live under web/ and are intended for this fork.
# Original artifacts are generated from the last pre-fork commit and copied into web/.
#
# Requirements (host):
# - bun / bash / python3
# - build-essential (for Musashi code generation)
# - uv + PlatformIO (or equivalent pio on PATH)
# - esptool (for merge/flash helper flows)
# - docker (optional, for Retro68 Mac app builds and HFS disk image updates)

SHELL := /bin/bash

WORKTREE_ORIGINAL := /workspace/tmp/cydintosh-upstream
UPSTREAM_BASE_COMMIT := cd5a6b2
PIO := /home/agent/.local/share/uv/tools/platformio/bin/pio
ESPTOOL := /home/agent/.local/share/uv/tools/esptool/bin/esptool
RETRO68_IMAGE := ghcr.io/autc04/retro68
SERIAL_PORT ?= /dev/cu.usbserial-210
BAUD ?= 115200

.PHONY: help prepare build firmware fs stable-artifacts flash-stable flash-stable-fs flash-stable-all \
	original-worktree original-build original-artifacts flash-original flash-original-fs flash-original-all \
	build-office-lights disk-update clean

help:
	@echo "Cydintosh build/flash targets"
	@echo ""
	@echo "Custom/fork targets:"
	@echo "  make prepare             - init submodules, generate Musashi ops, seed user_config.h and data/disk.img"
	@echo "  make build               - build firmware in current tree"
	@echo "  make firmware            - alias for make build"
	@echo "  make fs                  - generate LittleFS image in current tree (.pio/build/esp32dev/littlefs.bin)"
	@echo "  make stable-artifacts    - refresh fork stable artifacts in web/"
	@echo "  make flash-stable        - flash merged stable firmware only"
	@echo "  make flash-stable-fs     - flash stable filesystem only"
	@echo "  make flash-stable-all    - flash stable firmware + filesystem"
	@echo ""
	@echo "Original/upstream-equivalent targets:"
	@echo "  make original-worktree   - create/update clean upstream-equivalent worktree at $(WORKTREE_ORIGINAL)"
	@echo "  make original-build      - build firmware + real LittleFS image in clean upstream-equivalent worktree"
	@echo "  make original-artifacts  - copy original merged firmware + filesystem into web/"
	@echo "  make flash-original      - flash original merged firmware only"
	@echo "  make flash-original-fs   - flash original filesystem only"
	@echo "  make flash-original-all  - flash original firmware + filesystem"
	@echo ""
	@echo "Mac app / disk targets:"
	@echo "  make build-office-lights - build OfficeLights Mac app via Retro68 Docker image"
	@echo "  make disk-update         - update data/disk.img using Retro68 container HFS tools"
	@echo ""
	@echo "Variables: SERIAL_PORT=$(SERIAL_PORT) BAUD=$(BAUD)"

prepare:
	git submodule update --init --recursive
	ln -sf ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	make -C external/umac prepare
	@test -f include/user_config.h || cp include/user_config.h.tmpl include/user_config.h
	@mkdir -p data
	@test -f data/disk.img || cp cyd_800k.dsk data/disk.img

build firmware:
	$(PIO) run

fs:
	$(PIO) run -t uploadfs --disable-auto-clean || true
	@test -f .pio/build/esp32dev/littlefs.bin

stable-artifacts: build fs
	@mkdir -p web web/mac-apps
	cp .pio/build/esp32dev/bootloader.bin web/bootloader-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/firmware.bin web/firmware-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/partitions.bin web/partitions-stable-mqtt-v1.bin
	cp .pio/build/esp32dev/littlefs.bin web/littlefs-stable-mqtt-v1.bin
	$(ESPTOOL) --chip esp32 merge-bin -o web/merged-firmware-stable-mqtt-v1.bin \
	  --flash-mode dio --flash-freq 40m --flash-size 4MB \
	  0x1000 web/bootloader-stable-mqtt-v1.bin \
	  0x8000 web/partitions-stable-mqtt-v1.bin \
	  0x10000 web/firmware-stable-mqtt-v1.bin \
	  0x210000 rom_patched.bin
	@test -f mac-app/OfficeLightsApp/build/OfficeLights.bin && cp mac-app/OfficeLightsApp/build/OfficeLights.bin web/mac-apps/OfficeLights-stable-mqtt-v1.bin || true
	@echo "Stable artifacts refreshed under web/."

flash-stable:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/merged-firmware-stable-mqtt-v1.bin

flash-stable-fs:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x230000 web/littlefs-stable-mqtt-v1.bin

flash-stable-all:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/merged-firmware-stable-mqtt-v1.bin \
	  0x230000 web/littlefs-stable-mqtt-v1.bin

original-worktree:
	@if [ ! -d "$(WORKTREE_ORIGINAL)" ]; then \
	  git worktree add $(WORKTREE_ORIGINAL) $(UPSTREAM_BASE_COMMIT); \
	fi
	cd $(WORKTREE_ORIGINAL) && git submodule update --init --recursive
	cd $(WORKTREE_ORIGINAL) && sudo apt-get install -y build-essential >/dev/null
	cd $(WORKTREE_ORIGINAL) && ln -sf ../../../../include/m68kconf.h external/umac/external/Musashi/m68kconf.h
	cd $(WORKTREE_ORIGINAL) && make -C external/umac prepare
	cd $(WORKTREE_ORIGINAL) && cp include/user_config.h.tmpl include/user_config.h
	cd $(WORKTREE_ORIGINAL) && mkdir -p data && cp cyd_800k.dsk data/disk.img
	cd $(WORKTREE_ORIGINAL) && cp /workspace/projects/cydintosh/rom.bin rom.bin

original-build: original-worktree
	cd $(WORKTREE_ORIGINAL) && $(PIO) run
	cd $(WORKTREE_ORIGINAL) && $(PIO) run -t uploadfs --disable-auto-clean || true
	@test -f $(WORKTREE_ORIGINAL)/.pio/build/esp32dev/littlefs.bin
	cd $(WORKTREE_ORIGINAL) && python3 tools/generate_patched_rom.py rom.bin -o rom_patched.bin
	cd $(WORKTREE_ORIGINAL) && mkdir -p web
	cd $(WORKTREE_ORIGINAL) && cp .pio/build/esp32dev/bootloader.bin web/bootloader-original.bin
	cd $(WORKTREE_ORIGINAL) && cp .pio/build/esp32dev/firmware.bin web/firmware-original.bin
	cd $(WORKTREE_ORIGINAL) && cp .pio/build/esp32dev/partitions.bin web/partitions-original.bin
	cd $(WORKTREE_ORIGINAL) && cp .pio/build/esp32dev/littlefs.bin web/littlefs-original.bin
	cd $(WORKTREE_ORIGINAL) && $(ESPTOOL) --chip esp32 merge-bin -o web/merged-firmware-original.bin \
	  --flash-mode dio --flash-freq 40m --flash-size 4MB \
	  0x1000 web/bootloader-original.bin \
	  0x8000 web/partitions-original.bin \
	  0x10000 web/firmware-original.bin \
	  0x210000 rom_patched.bin

original-artifacts: original-build
	cp $(WORKTREE_ORIGINAL)/web/merged-firmware-original.bin web/merged-firmware-original.bin
	cp $(WORKTREE_ORIGINAL)/web/littlefs-original.bin web/littlefs-original.bin
	@echo "Original artifacts refreshed under web/."

flash-original:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/merged-firmware-original.bin

flash-original-fs:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x230000 web/littlefs-original.bin

flash-original-all:
	$(ESPTOOL) --port $(SERIAL_PORT) --baud $(BAUD) write_flash \
	  0x0000 web/merged-firmware-original.bin \
	  0x230000 web/littlefs-original.bin

build-office-lights:
	sudo docker run --rm --user $$(id -u):$$(id -g) -v /workspace/projects/cydintosh/mac-app:/root -i $(RETRO68_IMAGE) /bin/bash -lc '\
	set -e; \
	cd /root/OfficeLightsApp; \
	rm -rf build; mkdir build; cd build; \
	cmake .. -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake; \
	make -j2'
	python3 tools/set_macbinary_flags.py mac-app/OfficeLightsApp/build/OfficeLights.bin +bndl

disk-update:
	sudo docker run --rm -v /workspace/projects/cydintosh:/work -w /work $(RETRO68_IMAGE) /bin/bash -lc '\
	set -e; \
	/Retro68-build/toolchain/bin/hmount data/disk.img; \
	/Retro68-build/toolchain/bin/hdel OfficeLights || true; \
	/Retro68-build/toolchain/bin/hcopy -m mac-app/OfficeLightsApp/build/OfficeLights.bin :OfficeLights; \
	/Retro68-build/toolchain/bin/humount'

clean:
	rm -rf web/*.bin web/mac-apps/*.bin 2>/dev/null || true
	@echo "Removed generated web artifacts (if any)."
