# PicoStation Multi House firmware (reverse engineered)

Open source reconstruction of the firmware inside `PicoStation_MH_V3.uf2` (PicoStation Multi House V3,
RP2040, `picostation_pico1`, built Feb 5 2026 with pico-sdk 2.1.1).

* The base is the public PicoStation C++ code (megavolt85, Apache-2.0), adjusted to what the V3 image does.
* The Multi House specific layer (V2 SD card reader, PS1 command handler with cheats and disc switching,
  config and music server, legacy loaders, fallback menu, clock and trace) is rewritten as readable C++
  from the Ghidra decompile of the original image.
* Behavior is covered by host tests (`make -C tests`, 4,515 checks). **Nothing was run on a console or a
  real SD card**, and the machine code is not byte identical to the original.
  Read [docs/RE_STATUS.md](docs/RE_STATUS.md) before flashing.

Releases contain the UF2 built by CI with the pinned toolchain (Arm GNU 14.2.Rel1, pico-sdk 2.1.1).
Flash by holding BOOTSEL while plugging the Pico and copying the `.uf2` to the `RPI-RP2` drive.

Unofficial and not affiliated with the authors credited in [NOTICE](NOTICE). Build: [docs/BUILDING.md](docs/BUILDING.md).
