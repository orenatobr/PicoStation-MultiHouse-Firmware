# PicoStation board diagnostic

A small Pico firmware that checks the board with the console unplugged and the Pico powered by USB alone.

1. Hold BOOTSEL, plug in USB, copy `picostation_diag.uf2` to the `RPI-RP2` drive.
2. Open the serial port (`screen /dev/cu.usbmodem* 115200`) and read the report, repeated every 5 seconds:
   * SD card on SPI1 (GP8 MISO, GP9 CS, GP10 SCK, GP11 MOSI): initialization, SDHC or SDSC, and blocks 0 and 5 (V2 headers A and B, `PSXRAW` magic, version, case and disc counts).
   * Si5351 clock chip on I2C0 (GP12 SDA, GP13 SCL): address scan and status register 0. It should answer at 0x60 when it is powered.
   * Level of each signal pin floating, pulled up and pulled down. With the console off most lines read low; GP18, GP21 and GP25 follow the pulls.
3. Flash the real firmware back afterwards.

Build: `cmake -S diag -B build-diag -G Ninja -DPICO_SDK_PATH=<pico-sdk 2.1.1> && cmake --build build-diag`.
