# Thin wrapper around build.sh (the script keeps every Go cache under tools/.toolchain/go/).
.PHONY: all host test sizes repro

host:
	./build.sh host

all:
	./build.sh all

test:
	./build.sh test

sizes:
	./build.sh sizes

repro:
	./build.sh repro
