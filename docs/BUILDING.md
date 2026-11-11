# Building

Requirements: CMake >= 3.24, Ninja, Arm GNU Toolchain 14.2.Rel1 (`arm-none-eabi-gcc`,
with newlib), pico-sdk 2.1.1 with its submodules (tinyusb), a host C/C++ compiler (picotool and
pioasm are built by the SDK on the fly).

```sh
git clone --depth 1 --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git
git -C pico-sdk submodule update --init --depth 1 lib/tinyusb
export PICO_SDK_PATH=$PWD/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release
cmake --build build
# build/picostation_pico1.uf2
```

Flash by holding BOOTSEL while plugging the Pico, then copy the `.uf2` to the `RPI-RP2` drive.
