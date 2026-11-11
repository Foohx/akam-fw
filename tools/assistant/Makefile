# Thin wrapper around build.sh (the script keeps every Go cache under tools/.toolchain/go/).
.PHONY: all host test vet-all sizes sizes-by-package repro notices

host:
	./build.sh host

all:
	./build.sh all

test:
	./build.sh test

vet-all:
	./build.sh vet-all

sizes:
	./build.sh sizes

sizes-by-package:
	./build.sh sizes --by-package

repro:
	./build.sh repro

notices:
	./build.sh notices
