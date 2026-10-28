# SPDX-License-Identifier: AGPL-3.0-or-later
# Thin wrapper around CMake: `make`, `make run`, `make test`, `make release`,
# `make clean`, and `make fresh` after unpacking a zip.
# CMake generates the real build files in $(BUILD_DIR).

BUILD_DIR  ?= build
BUILD_TYPE ?= Debug
CMAKE_ARGS ?=

.PHONY: all configure build run test release clean touch fresh

all: build

# Configure every time: instant when nothing changed, and it repairs a build
# folder left half-configured by an interrupted run.
configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_ARGS)

build: configure
	cmake --build $(BUILD_DIR) --parallel

# Writes test.png, test.svg and test.pdf in the project folder.
run: build
	./$(BUILD_DIR)/render2d_demo

# Builds and runs the tests; --output-on-failure shows the failing checks.
test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

release:
	$(MAKE) BUILD_DIR=build-release BUILD_TYPE=Release build

clean:
	rm -rf build build-release

# After unpacking a zip: its files keep the date they had on Claude's side,
# often OLDER than our last build, and ninja would then rebuild nothing.
# `touch` dates every source now; `fresh` does that, then the tests (Debug)
# and the Release.
touch:
	find src tests CMakeLists.txt -type f -exec touch {} +

fresh: touch
	$(MAKE) test
	$(MAKE) release
