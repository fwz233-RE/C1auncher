.PHONY: all clean build test format run

# Default target: build using CMake
all: build

# Incremental builds preserve dependency bootstrap work.
BUILD_DIR ?= build
JOBS ?= 2
build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DTERM_IME_BUILD_JOBS=$(JOBS)
	cmake --build $(BUILD_DIR) -j$(JOBS)

# Clean build artifacts
clean:
	rm -rf build

# Run tests
test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Format code using clang-format
format:
	find src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# Run the program (for quick testing)
run:
	./build/term-ime