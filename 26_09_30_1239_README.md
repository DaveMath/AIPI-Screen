# AIPI-Screen

Known-good ESP-IDF display bring-up and calibration firmware for the XORIGIN
AIPI Lite (`XY006PL01`). This repository records the exact ST7735-compatible
LCD configuration validated on physical hardware after correcting color order,
orientation, inversion, and visible edge-static problems.

The test firmware also checks the ES8311 control bus and GPIO42 button so one
small image can verify the board, display, codec connection, and primary input.
It requires no Wi-Fi, API key, bridge, or cloud service.

## Validated result

On boot:

- A full green screen means the ES8311 responded at I2C address `0x18`.
- A full red screen means the codec probe failed; the LCD is still operating.
- Hold the right-side GPIO42 button to show four equal vertical bars in this
  order: **red, green, blue, white**.
- Release the button to return to the codec result screen.

The fixed image fills all 128 x 128 pixels. It does not leave the narrow static
strip on the left or the short static strip at the upper-left edge produced by
incorrect controller offsets.

## Known-good display configuration

| Setting | Value |
|---|---:|
| Controller | ST7735-compatible |
| Logical size | 128 x 128 |
| SPI host | `SPI2_HOST` |
| SPI mode | `0` |
| SPI clock | 20 MHz |
| Pixel format | RGB565 / 16 bit (`COLMOD`, `0x3A` = `0x05`) |
| Memory access | `MADCTL`, `0x36` = `0x68` |
| Color order | BGR bit enabled by `MADCTL` |
| Orientation | XY swap + X mirror |
| Inversion | Off (`INVOFF`, `0x20`) |
| X offset | `0` |
| Y offset | `0` |
| Pixel byte order | RGB565 words byte-swapped before SPI transfer |

See [DISPLAY_FIX.md](DISPLAY_FIX.md) for the symptoms, register-level fix, and
porting checklist.

## Pin map

| Function | ESP32-S3 pin |
|---|---:|
| LCD backlight | GPIO3 |
| ES8311 I2C SCL | GPIO4 |
| ES8311 I2C SDA | GPIO5 |
| LCD D/C | GPIO7 |
| LCD CS | GPIO15 |
| LCD SCLK | GPIO16 |
| LCD MOSI | GPIO17 |
| LCD reset | GPIO18 |
| Right-side button | GPIO42, active-low |

The broader AIPI Lite community pin map also identifies GPIO6 and GPIO11-14
for ES8311 I2S audio, GPIO9 for the speaker amplifier, and GPIO46 for the
WS2812 status LED. This repository deliberately leaves those signals alone.

## Requirements

- XORIGIN AIPI Lite / AIPI Lite with ESP32-S3
- USB data cable
- ESP-IDF 5.x; the hardware validation used ESP-IDF 5.5.4
- Python environment installed by ESP-IDF

## Build and flash

Load ESP-IDF, then build from the repository root:

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py build
```

Find the serial port on macOS:

```sh
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```

Flash and monitor, replacing the port if needed:

```sh
idf.py -p /dev/cu.usbmodem1301 flash monitor
```

This project pins `IDF_TARGET` to `esp32s3`, preventing the common failure where
the flash tool is launched with `--chip esp32` for this ESP32-S3 board.

Press `Ctrl-C` to exit the serial monitor. The repository-local
`esp-idf-monitor.cfg` changes the monitor exit key only for this project.

## Expected serial output

```text
I (...) aipi_screen: AIPI-Screen hardware test
I (...) aipi_screen: chip model=... cores=2 revision=... flash=16MB
I (...) aipi_screen: ES8311 at 0x18: PASS
I (...) aipi_screen: LCD initialized; green means codec found, red means codec missing
I (...) aipi_screen: Hold GPIO42 for RED | GREEN | BLUE | WHITE calibration bars
```

Pressing and releasing the right-side button adds:

```text
I (...) aipi_screen: GPIO42: PRESSED
I (...) aipi_screen: GPIO42: RELEASED
```

## Project layout

```text
.
|-- CMakeLists.txt             ESP32-S3 project definition
|-- DISPLAY_FIX.md            Register-level display reference
|-- esp-idf-monitor.cfg       Ctrl-C monitor exit setting
|-- main/
|   |-- CMakeLists.txt
|   `-- main.cpp              Hardware test and direct LCD driver
`-- sdkconfig.defaults        Flash, USB console, and logging defaults
```

## Community references

- [WalkieClaw](https://github.com/slsah30/WalkieClaw) for AIPI Lite application
  patterns and the 128 x 128 display context.
- [AIPI-Lite-ESPHome](https://github.com/sticks918/AIPI-Lite-ESPHome) for the
  board pin map and ESPHome bring-up.
- [AIPI-Lite-Voice-Bridge](https://github.com/noise754/AIPI-Lite-Voice-Bridge)
  for audio and board-control findings.
- [xiaozhi-esp32 AIPI Lite board support](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/xorigin/aipi-lite)
  for the upstream board definition.

## Origin

The test began as the hardware-isolation image in
[DaveMath/AiPiMusey](https://github.com/DaveMath/AiPiMusey). The display values
in this repository were then physically validated: full-screen coverage and
the red/green/blue/white sequence both passed.

