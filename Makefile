# Root entry point of the Akam firmware build. Thin wrapper: every target delegates to the per-model Makefile or CI script
# that already exists (those keep working on their own, see docs/manual/build.md). Nothing here changes how a binary is built.
#
# The pinned toolchain (tools/build/bootstrap.sh) is put on PATH automatically when present.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ZIGDIR := $(firstword $(wildcard $(ROOT)/tools/.toolchain/zig-*-linux*))
export PATH := $(if $(ZIGDIR),$(ZIGDIR):)$(PATH)
APP2K := $(ROOT)/firmware/lsc-rotatable-2k-ak3918av130
PLAT2K := $(ROOT)/platforms/anyka-ak3918av130

.DEFAULT_GOAL := help
.PHONY: help 1080p 2k 2k-appfs test check lint repro repro-1080p repro-2k clean

help:
	@echo "make 1080p     LSC indoor 1080p: akamd, flash_tool, helpers (everything under build/lsc-indoor-1080p-ak3918)"
	@echo "make 2k        LSC Rotatable 2K own app (platforms/anyka-ak3918av130) built in build/lsc-rotatable-2k-ak3918av130, published to dist/rot2k-app; needs CAMERA_LIBC=<libuClibc-*.so of your camera backup>"
	@echo "make 2k-appfs  card-less 2K appfs image (after make 2k); needs BACKUP=<stock mtd3 dump>"
	@echo "make test      shell, native and 2K test groups"
	@echo "make lint      shellcheck, hygiene"
	@echo "make check     the full local CI (tools/ci/local_check.sh)"
	@echo "make repro     both reproducibility checks against tests/golden (repro-1080p, repro-2k)"
	@echo "make clean     remove build/ and the build outputs of both models"

1080p:
	$(MAKE) -C $(ROOT)/platforms/anyka-ak3918 all

2k:
	@test -n "$(CAMERA_LIBC)" || { echo "usage: make 2k CAMERA_LIBC=<libuClibc-*.so from your own camera backup>" >&2; exit 1; }
	$(MAKE) -C $(PLAT2K) poc3-dist CAMERA_LIBC="$(CAMERA_LIBC)"

2k-appfs:
	@test -n "$(BACKUP)" || { echo "usage: make 2k-appfs BACKUP=<stock mtd3 dump>  (after make 2k)" >&2; exit 1; }
	bash $(APP2K)/image/build_appfs.sh --backup "$(BACKUP)"

test:
	$(ROOT)/tools/ci/local_check.sh --only shell,native,rot2k

check:
	$(ROOT)/tools/ci/local_check.sh

lint:
	$(ROOT)/tools/ci/local_check.sh --only lint

repro: repro-1080p repro-2k

repro-1080p:
	$(ROOT)/tools/ci/repro_check.sh

repro-2k:
	$(ROOT)/tools/ci/repro_check_rot2k.sh

clean:
	rm -rf $(ROOT)/build
	$(MAKE) -C $(ROOT)/platforms/anyka-ak3918 clean
	$(MAKE) -C $(PLAT2K) clean
