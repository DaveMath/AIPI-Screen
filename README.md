# AIPI-Screen

Known-good ESP-IDF display bring-up, calibration, and phone-based Wi-Fi setup
firmware for the XORIGIN AIPI Lite (`XY006PL01`). This repository records the
exact ST7735-compatible LCD configuration validated on physical hardware after
correcting color order, orientation, inversion, and visible edge-static
problems.

The test firmware also checks the ES8311 control bus and GPIO42 button, then
starts a captive setup portal with a nearby-network scanner. It requires no API
key, bridge, or cloud service.

## A lot of hardware for about $20

For roughly $20, this little board combines an ESP32-S3, 16 MB flash, 8 MB
PSRAM, a 128 x 128 color display, ES8311 audio codec, MEMS microphone, speaker,
two buttons, a WS2812 status LED, Wi-Fi, USB-C, and optional battery operation.
That makes it unusually versatile for voice assistants, local AI interfaces,
status displays, smart-home controls, notification devices, sensor dashboards,
and compact network tools.

[Purchase the AIPI Lite from the AIPI Amazon store](https://www.amazon.com/stores/page/C88F1EA2-88AD-4BED-84F0-046A3DC763AA?ingress=2&lp_context_asin=B0FQNNVV36&visitId=356e09b1-3791-4637-9b0d-8cc265449ec1&ref_=ast_bln)

Firmware, testing, and documentation by **@GGDM**.

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

The table distinguishes settings verified by this repository on physical
hardware from settings imported from working community firmware that still
need to be exercised by this diagnostic image.

| Subsystem | Function | ESP32-S3 pin | Electrical/configuration detail | Status here |
|---|---|---:|---|---|
| Display | Backlight | GPIO3 | Active-high | Validated |
| Display | D/C | GPIO7 | SPI data/command select | Validated |
| Display | CS | GPIO15 | Active-low SPI chip select | Validated |
| Display | SCLK | GPIO16 | SPI clock, validated at 20 MHz | Validated |
| Display | MOSI | GPIO17 | SPI output to LCD | Validated |
| Display | Reset | GPIO18 | Active-low | Validated |
| Audio control | ES8311 SCL | GPIO4 | I2C, community firmware uses 400 kHz | Address validated |
| Audio control | ES8311 SDA | GPIO5 | I2C address `0x18` | Address validated |
| Audio stream | MCLK | GPIO6 | I2S master clock | Community verified |
| Audio stream | speaker DOUT | GPIO11 | ESP32 to ES8311 | Community verified |
| Audio stream | LRCLK / WS | GPIO12 | Shared microphone/speaker bus | Community verified |
| Audio stream | microphone DIN | GPIO13 | ES8311 to ESP32 | Community verified |
| Audio stream | BCLK | GPIO14 | Shared microphone/speaker bus | Community verified |
| Audio output | Speaker amplifier | GPIO9 | Disable while recording | Community verified |
| Input | Right/talk button | GPIO42 | Active-low | Validated |
| Input | Left/volume button | GPIO1 | Active-low, internal pull-up | Community verified |
| Status | WS2812 data | GPIO46 | One LED, GRB element order | Community verified |
| Power | Power hold | GPIO10 | Drive high to remain on when battery powered | Community verified |
| Power | Battery voltage | GPIO2 | ADC; starting divider multiplier `2.5` | Community verified |
| Power | Charge status | GPIO8 | Active-low input with pull-up | Community verified |

`Community verified` means the setting appears in a working AIPI Lite project,
especially WalkieClaw, but has not yet passed this repository's own physical
acceptance test. GPIO10 deserves special care: changing the power-hold signal
can switch the device off when it is running from the battery module.

## Two-button controls

The board provides enough input for a useful interface without adding touch or
external controls:

| Input | Pin | Proven behavior | Recommended native-firmware behavior |
|---|---:|---|---|
| Right button | GPIO42 | Push-to-talk, active-low | Hold for primary action; release to submit |
| Left button | GPIO1 | Volume cycle, active-low | Short press cycles volume or brightness |
| Both buttons | GPIO42 + GPIO1 | Diagnostic overlay | Brief press shows device information |
| Both, long hold | GPIO42 + GPIO1 | Not defined upstream | Clear Wi-Fi configuration after confirmation |

WalkieClaw debounces GPIO42 by 200 ms in both directions and GPIO1 by 100 ms.
Its right-button behavior is particularly useful for an assistant state
machine: pressing while idle starts recording, pressing during playback
interrupts the audio, and pressing while processing abandons the old response
before beginning a new recording.

Its left button uses a simple persistent volume sequence:

```text
Off -> 45% -> 60% -> 70% -> 80% -> Off
```

For this repository, the next diagnostic should use GPIO1 to advance through
hardware-test pages and reserve GPIO42 for the action on each page. A brief
two-button press can show firmware version, station IP, RSSI, battery voltage,
charge state, codec status, and PSRAM size. A destructive factory reset should
require a long hold and an on-screen countdown so it cannot happen accidentally.

## Audio configuration

The ES8311 is both the microphone ADC and speaker DAC. The current image proves
only that its control interface responds at I2C address `0x18`; it does not yet
prove acoustic microphone capture or audible playback.

Known working community settings:

| Setting | Value |
|---|---|
| Sample rate | 16 kHz |
| Sample format | Signed 16-bit PCM |
| Channels | Mono |
| Codec control | I2C address `0x18` |
| I2S pins | MCLK 6, DOUT 11, LRCLK 12, DIN 13, BCLK 14 |
| Speaker gate | GPIO9 |

The microphone and speaker share clocks and codec state, so mode changes need
an explicit sequence. A conservative transition based on WalkieClaw is:

1. Stop playback or wake-word processing.
2. Disable the GPIO9 speaker amplifier.
3. Wait 100 ms before microphone capture.
4. Stop capture before changing the I2S direction.
5. Wait about 500 ms before enabling the speaker amplifier.
6. Restore the ES8311 DAC state, then wait 100-300 ms before playback.

WalkieClaw restores DAC volume/control with ES8311 registers `0x32` and `0x37`
before playback. Those writes should be verified against our board and codec
initialization before they become part of the native driver. Do not write
unrelated ES8311 registers speculatively; microphone and DAC paths share state.

## LED and status language

GPIO46 drives one WS2812-compatible LED using **GRB** order. A useful common
status vocabulary is:

| State | LED | Screen |
|---|---|---|
| Booting | Blue | Hardware checks |
| Ready | Green | Ready / connected |
| Listening | Red | Live microphone level |
| Processing | Blue | Waiting for response |
| Speaking | Orange | Response text |
| Setup required | Amber pulse | Setup SSID and `1.2.3.4` |
| Error | Red pulse | Short fault and recovery action |

Brightness should stay near 15% for normal status use. A single WS2812 at full
brightness is distracting and wastes battery in a device this small.

## Power and battery

GPIO10 is the battery power-hold control. Battery-oriented firmware should
assert it early in startup and keep it high until an intentional shutdown.
Dropping it can remove power from the board. USB-powered display development
may hide this behavior, so it needs a separate unplugged test.

WalkieClaw reads battery voltage through GPIO2 with 12 dB ADC attenuation, ten
samples, and a starting voltage-divider multiplier of `2.5`. That multiplier
must be calibrated against a multimeter for each hardware revision. Its
piecewise LiPo estimate uses these reference points:

| Cell voltage | Approximate charge |
|---:|---:|
| 4.20 V | 100% |
| 4.10 V | 90% |
| 3.95 V | 70% |
| 3.80 V | 50% |
| 3.70 V | 30% |
| 3.50 V | 10% |
| 3.30 V | 0% |

GPIO8 reports charging state as an active-low input. Useful behavior is to
refresh battery data when that state changes, blink the battery indicator while
charging below 50%, and reset the sleep timer when external power is removed.

WalkieClaw's configured sleep timeout is `1,800,000 ms`, which is 30 minutes,
despite one nearby comment calling it five minutes. Native code should define
the duration once and derive both behavior and display text from that value.

## Memory and platform settings

The AIPI Lite uses an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM. Community
firmware enables octal PSRAM at 80 MHz with a 64 KB data cache and 64-byte cache
lines. Those settings matter once the firmware adds frame buffers, audio queues,
TLS, wake-word models, or larger web assets.

| Platform setting | Value |
|---|---|
| ESP-IDF target | `esp32s3` |
| Flash size | 16 MB |
| Flash mode | DIO |
| Flash frequency | 80 MHz |
| PSRAM | 8 MB octal, 80 MHz |
| USB console | USB Serial/JTAG |
| Display transfer buffer | Eight RGB565 rows in the current driver |

The current repository pins the correct chip and flash settings. PSRAM is not
required by the small calibration image and is not yet enabled by its default
configuration.

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

## Phone Wi-Fi setup

After boot, the AIPI creates an open setup network named
`AIPI-Screen-XXXX`, where `XXXX` comes from the device MAC address.

1. Open Wi-Fi settings on a phone.
2. Join `AIPI-Screen-XXXX`.
3. Accept the captive-portal prompt. If it does not open automatically, browse
   directly to [http://1.2.3.4](http://1.2.3.4).
4. The device scans nearby 2.4 GHz Wi-Fi networks and sorts them by signal
   strength.
5. Select the desired network, enter its password, and tap **Save and connect**.

The password field includes a Show/Hide control so it can be checked before
saving. The SSID and password are stored only in ESP32 NVS and are never sent
to an external service or returned by the portal. The setup AP remains
available so the network can be changed later without reflashing.

Technical details:

| Provisioning setting | Value |
|---|---|
| Setup SSID | `AIPI-Screen-XXXX` |
| Setup security | Open, local configuration only |
| Portal and gateway | `1.2.3.4` |
| Netmask | `255.255.255.0` |
| Wi-Fi mode | AP + station |
| Network discovery | On-demand active scan, strongest first |
| Credential storage | ESP-IDF NVS namespace `aipi_screen` |
| Captive detection | Local DNS redirects host lookups to `1.2.3.4` |

WalkieClaw also demonstrates two-network roaming: a home network at higher
priority and a phone hotspot as fallback, with Wi-Fi power saving disabled for
reliable real-time audio. That is a good production direction for AIPI, but the
current native portal stores one station network. A future settings schema
should support at least two saved networks without exposing their passwords.

The present `AIPI-Screen-XXXX` setup AP is intentionally open for bring-up.
Production firmware should derive a unique setup password from device-specific
data, show it on the LCD, and disable or time-limit the AP after provisioning.
A shared password such as `12345678` is convenient for development but not an
acceptable shipping default.

## Suggested settings model

The device has enough persistent configuration to justify a versioned NVS
record instead of unrelated keys scattered through application code:

| Setting | Type/default | Notes |
|---|---|---|
| Wi-Fi networks | Up to two SSID/password pairs | Home plus mobile hotspot |
| Device name | `AIPI-Screen-XXXX` | Use MAC suffix for uniqueness |
| Setup AP enabled | `true` until provisioned | Reopen with button gesture |
| Display brightness | 70% | Change with left button in display mode |
| Speaker volume | 70% | Persist last selected step |
| Sleep timeout | 30 minutes | Apply only on battery and while idle |
| Wake word | Disabled | Saves power and avoids unwanted capture |
| Diagnostic logging | Info | Debug should be an explicit option |

Configuration writes should be atomic and schema-versioned. Passwords and API
keys must never appear in logs, status pages, screenshots, or JSON responses.
Wi-Fi reset and full factory reset should remain separate operations.

## Diagnostic acceptance plan

The display and GPIO42 portions have passed. The fuller board test should add
these pages in order:

1. **Display:** edge coverage and red/green/blue/white bars.
2. **Buttons:** independent GPIO1/GPIO42 state plus simultaneous press.
3. **LED:** red, green, blue, white at limited brightness.
4. **Power:** GPIO10 hold behavior on battery and USB.
5. **Battery:** GPIO2 raw voltage, calibrated voltage, percent, GPIO8 charge state.
6. **Codec:** I2C address and selected ES8311 register readback.
7. **Microphone:** live peak/RMS meter without speaker enabled.
8. **Speaker:** short generated tone after the safe audio transition.
9. **Memory:** internal heap, PSRAM presence, size, and allocation test.
10. **Network:** AP address, station address, RSSI, scan count, and NVS reload.

Each page should display `PASS`, `FAIL`, or `NOT TESTED`; community-derived
values should not silently become “validated” until the corresponding physical
test passes.

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
|   |-- main.cpp              Hardware test and direct LCD driver
|   |-- wifi_portal.cpp       Captive portal, scanner, and NVS settings
|   `-- wifi_portal.h
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
the red/green/blue/white sequence both passed. Project work is credited to
**@GGDM**.
