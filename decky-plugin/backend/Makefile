# Builds ds5bridged into ./out (the Decky CLI copies ./out to the plugin's bin/).
BUILD_DIR ?= build-release
OUT_DIR ?= out

.PHONY: all test clean

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j$$(nproc) --target ds5bridged ds5bridge_testtool
	mkdir -p $(OUT_DIR)
	cp $(BUILD_DIR)/ds5bridged $(OUT_DIR)/ds5bridged
	strip $(OUT_DIR)/ds5bridged || true

test: all
	cd tests && DS5BRIDGE_BUILD=$(abspath $(BUILD_DIR)) python3 -m unittest -v test_bridge

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
