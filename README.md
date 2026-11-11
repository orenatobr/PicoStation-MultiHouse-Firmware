# PicoStation Multi House firmware (reverse engineered)

Open source reconstruction of the firmware inside `PicoStation_MH_V3.uf2` (PicoStation Multi
House V3, RP2040 / `picostation_pico1`, built Feb 5 2026, pico-sdk 2.1.1).

**Status: work in progress.** The tree starts from the public PicoStation C++ code base that the
V3 image was built from, and the Multi House specific parts (V2 card reader, command handler,
cheats, config and music servers, fallback menu) are being rebuilt from the Ghidra decompile
of the original image. Nothing in this repository has been run on a console or a real SD card.
See [docs/RE_STATUS.md](docs/RE_STATUS.md) for what is matched against the original and what is not.

Unofficial. Credits and licenses: see [NOTICE](NOTICE). Build: [docs/BUILDING.md](docs/BUILDING.md).
