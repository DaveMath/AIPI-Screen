# AIPI-Screen

Known-good ESP-IDF display bring-up, calibration, and phone-based Wi-Fi setup
firmware for the XORIGIN AIPI Lite (`XY006PL01`). This repository records the
exact ST7735-compatible LCD configuration validated on physical hardware after
correcting color order, orientation, inversion, and visible edge-static
problems.

The test firmware also checks the ES8311 control bus and both onboard buttons, then
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

**Free by @GGDM.** Firmware, testing, and documentation by **@GGDM**, with no
subscription, bridge, or cloud account required for the hardware test and
local setup features. The repository is released under the included MIT
license.

## Validated result

On boot:

- A full green screen means the ES8311 responded at I2C address `0x18`.
- A full red screen means the codec probe failed; the LCD is still operating.
- Hold the left-side GPIO1 button to show a full yellow screen.
- Hold the right-side GPIO42 button to show four equal vertical bars in this
  order: **red, green, blue, white**.
- Hold both buttons to show yellow over magenta.
- Release the buttons to return to the codec result screen.
- The bottom battery bar is green at 50% or above, yellow from 10-49%, and
  red below 10%. A white marker means GPIO8 reports charging; below 50% the
  bar blinks every 600 ms while charging.

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
| Input | Left/volume button | GPIO1 | Active-low, internal pull-up | Implemented; physical retest pending |
| Status | WS2812 data | GPIO46 | One LED, GRB element order | Community verified |
| Power | Power hold | GPIO10 | Driven high at startup | Implemented; battery retest pending |
| Power | Battery voltage | GPIO2 | ADC1 channel 1, 12 dB, 10-sample average, multiplier `2.5` | Implemented; calibration pending |
| Power | Charge status | GPIO8 | Active-low input with pull-up | Implemented; battery retest pending |

`Community verified` means the setting appears in working AIPI Lite firmware
but has not yet passed this repository's own physical acceptance test. GPIO10
deserves special care: changing the power-hold signal can switch the device off
when it is running from the battery module.

## Two-button controls

The board provides enough input for a useful interface without adding touch or
external controls:

| Input | Pin | Proven behavior | Recommended native-firmware behavior |
|---|---:|---|---|
| Right button | GPIO42 | Push-to-talk, active-low | Hold for primary action; release to submit |
| Left button | GPIO1 | Volume cycle, active-low | Short press cycles volume or brightness |
| Both buttons | GPIO42 + GPIO1 | Diagnostic overlay | Brief press shows device information |
| Both, long hold | GPIO42 + GPIO1 | Not defined upstream | Clear Wi-Fi configuration after confirmation |

The reference control module debounces GPIO42 by 200 ms in both directions and
GPIO1 by 100 ms. A useful assistant state machine can start recording from idle,
interrupt playback, or abandon processing before beginning a new recording.

Its left button uses a simple persistent volume sequence:

```text
Off -> 45% -> 60% -> 70% -> 80% -> Off
```

The diagnostic firmware now reads both buttons through `aipi_controls` and
changes the display after each accepted press or release. GPIO1 shows yellow,
GPIO42 shows the RGBW calibration bars, and holding both shows yellow over
magenta. A brief
two-button press can show firmware version, station IP, RSSI, battery voltage,
charge state, codec status, and PSRAM size. A destructive factory reset should
require a long hold and an on-screen countdown so it cannot happen accidentally.

### Native GPIO addressing rule

Arduino-style numeric `pinMode()` and `digitalRead()` calls can pass through a
board-variant mapping instead of addressing the ESP32-S3 GPIO expected by the
AiPi schematic. The result can be a clean build and buttons that never change
the application state.

AiPi firmware must use the native ESP-IDF GPIO API for these controls:

```cpp
ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_1));
ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_42));

gpio_config_t inputs = {};
inputs.pin_bit_mask = (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_42);
inputs.mode = GPIO_MODE_INPUT;
inputs.pull_up_en = GPIO_PULLUP_ENABLE;
inputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
inputs.intr_type = GPIO_INTR_DISABLE;
ESP_ERROR_CHECK(gpio_config(&inputs));
ESP_ERROR_CHECK(gpio_pullup_en(GPIO_NUM_1));
ESP_ERROR_CHECK(gpio_pullup_en(GPIO_NUM_42));

const bool left_pressed = gpio_get_level(GPIO_NUM_1) == 0;
const bool right_pressed = gpio_get_level(GPIO_NUM_42) == 0;
```

Both inputs are active-low: idle is `1` and pressed is `0`. Log both raw levels
at boot, debounce press and release, and force the visible UI state to redraw
when a debounced transition is accepted. This distinguishes a wiring or pin-map
problem from an application state that changed without being rendered.

### Cross-project button regression finding

An AiPi application can build, flash, and render normally while both controls
appear dead if GPIO1 or GPIO42 retains an earlier boot/debug pad configuration.
The reusable initialization sequence now resets both pads, configures them
together as active-low inputs, and explicitly re-enables both pull-ups. Both
buttons use a 35 ms debounce interval so a normal short press is accepted
without making the UI feel delayed.

Every raw electrical transition is logged before debounce. Validation should
therefore happen in this order:

1. Confirm boot reports idle levels `GPIO1=1` and `GPIO42=1`.
2. Confirm a press logs raw level `0` and release logs raw level `1`.
3. Confirm the debounced state changes after at least 35 ms.
4. Confirm the application redraws the display from the accepted transition.

GPIO42 is physically validated as the right button. GPIO1 is the established
left-button mapping but remains flagged for a direct physical retest. The
reset-and-reassert sequence is a regression hardening change discovered while
integrating battery monitoring in another AiPi firmware and also requires a
final physical retest on each board revision. Battery GPIO2 ADC, GPIO8 charge
status, and GPIO10 power hold do not overlap either button pin.

## Audio configuration

The ES8311 is both the microphone ADC and speaker DAC. Its control interface at
I2C address `0x18` and its speaker output path are physically validated. The
native diagnostic now cycles `MUTE`, `10%`, `50%`, and `100%` on GPIO42 presses:
10% plays a short chirp, 50% plays a two-pulse beep, and 100% plays a rising
two-tone alert. Microphone capture remains a separate validation item.

Known working community settings:

| Setting | Value |
|---|---|
| Sample rate | 16 kHz |
| Sample format | Signed 16-bit PCM |
| Channels | Duplicated mono in 16-bit stereo I2S slots |
| Codec control | I2C address `0x18` |
| I2S pins | MCLK 6, DOUT 11, LRCLK 12, DIN 13, BCLK 14 |
| Speaker gate | GPIO9 |

The validated speaker implementation derives the ES8311 clock from the 64x BCLK
and does not drive GPIO6 MCLK. GPIO9 is high only during bounded playback; the
DAC is muted and the amplifier is low while idle. A 40 ms silence tail plus a
60 ms DMA-drain delay prevents the final short tone from being cut off.

The microphone and speaker share clocks and codec state, so mode changes need
an explicit sequence. A conservative transition is:

1. Stop playback or wake-word processing.
2. Disable the GPIO9 speaker amplifier.
3. Wait 100 ms before microphone capture.
4. Stop capture before changing the I2S direction.
5. Wait about 500 ms before enabling the speaker amplifier.
6. Restore the ES8311 DAC state, then wait 100-300 ms before playback.

The native driver initializes DAC volume/control with ES8311 registers `0x32`
and `0x37`, streams signed 16-bit PCM on GPIO11, and gates the amplifier on
GPIO9. Do not write unrelated ES8311 registers speculatively; microphone and
DAC paths share state.

## LED color control and status language

GPIO46 drives one WS2812-compatible LED using **GRB** wire order. The setup
portal includes a native color picker, five useful preset swatches, a live
preview button, and a brightness slider limited to 0-40%. Color and brightness
are saved in NVS and restored at startup. The default is @GGDM gold at 15%,
which is visible without turning a tiny status light into a desk lamp.

A useful common status vocabulary is:

| State | LED | Screen |
|---|---|---|
| Booting | Blue | Hardware checks |
| Ready | Green | Ready / connected |
| Listening | Red | Live microphone level |
| Processing | Blue | Waiting for response |
| Speaking | Orange | Response text |
| Setup required | Amber pulse | Setup SSID and `1.2.3.4` |
| Error | Red pulse | Short fault and recovery action |

Brightness should stay near 15% for normal status use. The 40% portal ceiling
keeps accidental selections reasonable; firmware-controlled diagnostic effects
can still use other levels when required.

## Power and battery

GPIO10 is the battery power-hold control. Battery-oriented firmware should
assert it early in startup and keep it high until an intentional shutdown.
Dropping it can remove power from the board. USB-powered display development
may hide this behavior, so it needs a separate unplugged test.

The native `aipi_battery` module implements the board's battery model. It
asserts GPIO10 at startup, reads GPIO2 through ADC1 channel 1 with 12 dB
attenuation, averages ten samples, and applies a starting voltage-divider
multiplier of `2.5`. ESP-IDF curve-fitting calibration is used when available;
otherwise the driver reports that its voltage estimate is uncalibrated. The
`2.5` multiplier still must be checked against a multimeter for each hardware
revision. The piecewise LiPo estimate uses these reference points:

| Cell voltage | Approximate charge |
|---:|---:|
| 4.20 V | 100% |
| 4.10 V | 90% |
| 3.95 V | 70% |
| 3.80 V | 50% |
| 3.70 V | 30% |
| 3.50 V | 10% |
| 3.30 V | 0% |

GPIO8 reports charging state as an active-low input. Firmware samples the
battery every 30 seconds and immediately when charge state changes. The bottom
screen bar uses red/yellow/green thresholds and blinks every 600 ms
while charging below 50%. Serial output includes voltage, percentage, charging
state, and whether eFuse-backed ADC calibration was available.

The recommended sleep timeout is `1,800,000 ms`, or 30 minutes. Native code
should define the duration once and derive both behavior and display text from
that value.

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

After boot, the AIPI creates a WPA2 setup network named `AIPI-Screen-XXXX`,
where `XXXX` comes from the device MAC address. Its password is `GGDM-XXXX`,
using the same four-character suffix. This prevents accidental access during
setup; because the suffix is visible in the SSID, it is not intended to be a
high-security secret.

1. Open Wi-Fi settings on a phone.
2. Join `AIPI-Screen-XXXX` with password `GGDM-XXXX`.
3. Accept the captive-portal prompt. If it does not open automatically, browse
   directly to [http://1.2.3.4](http://1.2.3.4).
4. The device scans nearby 2.4 GHz Wi-Fi networks and sorts them by signal
   strength. A hidden SSID can be entered manually.
5. Select or enter the desired network, enter its password, choose the hotspot
   mode, set the LED color and brightness, then tap **Save settings and connect**.

The password field includes a Show/Hide control so it can be checked before
saving. The SSID and password are stored only in ESP32 NVS and are never sent
to an external service or returned by the portal. Plain NVS is local storage,
not encryption; production builds that need protection against physical flash
extraction should enable ESP-IDF NVS encryption and flash encryption.

**Setup only** is the recommended host mode. It disables the setup hotspot
after station Wi-Fi connects, while leaving the settings server reachable at
the device's LAN address. After five failed reconnect attempts the device
restores AP+station mode so it cannot strand itself. **Always on** keeps AP+
station mode active for development and bench work. Wi-Fi power saving is
disabled in both modes for responsive setup and future real-time audio.

Technical details:

| Provisioning setting | Value |
|---|---|
| Setup SSID | `AIPI-Screen-XXXX` |
| Setup password | `GGDM-XXXX`, matching the SSID suffix |
| Setup security | WPA2-PSK; casual-access guard, not a secret-grade credential |
| Portal and gateway | `1.2.3.4` |
| Netmask | `255.255.255.0` |
| Wi-Fi mode | Setup-only by default; optional persistent AP + station |
| Network discovery | On-demand active scan, strongest first |
| Hidden networks | Manual SSID entry |
| Credential storage | ESP-IDF NVS namespace `aipi_screen`; encryption optional |
| Captive detection | Local DNS redirects host lookups to `1.2.3.4` |
| Resilience | Setup AP restored after five failed station reconnects |

Two-network roaming, with a home network at higher priority and a phone hotspot
as fallback, remains a useful next step. The current native portal stores one
station network and recovers by reopening its setup AP when that network cannot
be reached.

## Suggested settings model

The device has enough persistent configuration to justify a versioned NVS
record instead of unrelated keys scattered through application code:

| Setting | Type/default | Notes |
|---|---|---|
| Wi-Fi networks | Up to two SSID/password pairs | Home plus mobile hotspot |
| Device name | `AIPI-Screen-XXXX` | Use MAC suffix for uniqueness |
| Setup host mode | Setup only | Always-on option for development |
| LED color | `#c9a84c` | Live portal preview, persisted in NVS |
| LED brightness | 15% | Portal range is 0-40% |
| Display brightness | 70% | Change with left button in display mode |
| Speaker volume | 70% | Persist last selected step |
| Sleep timeout | 30 minutes | Apply only on battery and while idle |
| Wake word | Disabled | Saves power and avoids unwanted capture |
| Diagnostic logging | Info | Debug should be an explicit option |

The Wi-Fi and LED settings above are implemented as individual NVS keys. Before
the settings surface grows further, migrate them to an atomic, schema-versioned
record. Passwords and API keys must never appear in logs, status pages,
screenshots, or JSON responses. Wi-Fi reset and full factory reset should
remain separate operations.

## Diagnostic acceptance plan

The display and native two-button input path are implemented. The fuller board
test should add these pages in order:

1. **Display:** edge coverage and red/green/blue/white bars.
2. **Buttons:** physically validate independent GPIO1/GPIO42 state plus simultaneous press.
3. **LED:** red, green, blue, white at limited brightness.
4. **Power:** physically validate GPIO10 hold behavior on battery and USB.
5. **Battery:** calibrate GPIO2 voltage and physically validate percentage and GPIO8 charge state.
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
I (...) aipi_screen: battery=4.012V percent=78 charging=0 adc_calibrated=1
I (...) aipi_screen: LCD initialized; green means codec found, red means codec missing
I (...) aipi_screen: buttons: GPIO1=yellow GPIO42=RGBW both=yellow/magenta
```

Pressing and releasing the buttons adds:

```text
I (...) aipi_screen: GPIO1 left: PRESSED
I (...) aipi_screen: GPIO1 left: RELEASED
I (...) aipi_screen: GPIO42 right: PRESSED
I (...) aipi_screen: GPIO42 right: RELEASED
```

## Project layout

```text
.
|-- CMakeLists.txt             ESP32-S3 project definition
|-- DISPLAY_FIX.md            Register-level display reference
|-- esp-idf-monitor.cfg       Ctrl-C monitor exit setting
|-- main/
|   |-- CMakeLists.txt
|   |-- aipi_battery.cpp      Power hold, ADC averaging, charge state, LiPo curve
|   |-- aipi_battery.h        Shared battery reading API
|   |-- aipi_controls.cpp     Native two-button GPIO and debounce implementation
|   |-- aipi_controls.h       Shared AiPi control pins and state API
|   |-- main.cpp              Hardware test and direct LCD driver
|   |-- status_led.cpp        GPIO46 WS2812 RMT driver and brightness control
|   |-- status_led.h
|   |-- wifi_portal.cpp       Captive portal, Wi-Fi/LED UI, and NVS settings
|   `-- wifi_portal.h
`-- sdkconfig.defaults        Flash, USB console, and logging defaults
```

## Community references

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
