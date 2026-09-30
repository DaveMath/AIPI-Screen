# AIPI Lite ST7735 display fix

## Problem signature

The display was electrically alive and accepted SPI data, but showed two
distinct classes of error:

1. Colors did not map to the requested RGB values.
2. Roughly four pixels of static remained down the left edge, with an
   additional short strip about forty pixels wide and four pixels high near
   the upper-left corner.

The color bars became correct as **red, green, blue, white**, but the static
edge pixels remained until the address offsets were removed.

## Confirmed fix

Use this controller setup:

```cpp
const uint8_t color_mode[] = {0x05};
lcd_command_data(0x3A, color_mode, sizeof(color_mode));  // RGB565

const uint8_t orientation[] = {0x68};
lcd_command_data(0x36, orientation, sizeof(orientation));

lcd_command(0x20);  // INVOFF
lcd_command(0x29);  // DISPON
```

Address the panel with no origin correction:

```cpp
const int column_start = x;
const int row_start = y;
```

Do not add the `+2`, `+1`, or other offsets commonly required by 128 x 128
ST7735 modules mounted on a larger controller RAM area. Those offsets caused
visible, unwritten edge regions on this AIPI Lite panel.

Finally, byte-swap each RGB565 value before sending a `uint16_t` pixel buffer
from the little-endian ESP32-S3:

```cpp
uint16_t wire_color = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
```

## What `MADCTL = 0x68` selects

`0x68` combines these ST7735 memory-access flags:

| Bit | Meaning | State |
|---|---|---|
| `MV` (`0x20`) | Exchange row and column addressing | On |
| `MX` (`0x40`) | Mirror X / column order | On |
| `BGR` (`0x08`) | BGR panel element order | On |
| `MY` (`0x80`) | Mirror Y / row order | Off |

Changing both `MADCTL` color order and application color constants can cancel
each other during one test and fail later with real images. Keep conventional
RGB565 constants (`0xF800`, `0x07E0`, `0x001F`, `0xFFFF`) and put the panel's
element order in `MADCTL`.

## Minimal initialization order

1. Hold reset low for 20 ms.
2. Release reset and wait 120 ms.
3. Send software reset (`SWRESET`, `0x01`) and wait 150 ms.
4. Send sleep out (`SLPOUT`, `0x11`) and wait 120 ms.
5. Set RGB565 (`COLMOD`, `0x3A`, data `0x05`).
6. Set orientation and element order (`MADCTL`, `0x36`, data `0x68`).
7. Disable inversion (`INVOFF`, `0x20`).
8. Turn the display on (`DISPON`, `0x29`) and wait 20 ms.
9. Enable the GPIO3 backlight.

Keeping the backlight off during setup prevents the user from seeing reset and
uninitialized controller memory.

## Window writes

For a rectangle at `(x, y)` with dimensions `(width, height)`:

```text
CASET (0x2A): x, x + width - 1
RASET (0x2B): y, y + height - 1
RAMWR (0x2C): width * height RGB565 pixels
```

All coordinates are sent as 16-bit big-endian values. Pixel data is also
big-endian on the wire. The test transfers eight rows per block to keep the
working buffer small while retaining straightforward full-screen writes.

## Calibration acceptance test

Hold GPIO42 and inspect four 32 x 128 bars:

| X range | RGB565 | Expected color |
|---|---:|---|
| 0-31 | `0xF800` | Red |
| 32-63 | `0x07E0` | Green |
| 64-95 | `0x001F` | Blue |
| 96-127 | `0xFFFF` | White |

The fix passes only when:

- the sequence is red, green, blue, white;
- all bars are equal width;
- the image reaches every edge;
- no static pixels remain along the left or top edges;
- press and release do not shift or corrupt the display.

## Troubleshooting matrix

| Symptom | First check |
|---|---|
| Red and blue exchanged | `MADCTL` BGR bit and RGB565 constants |
| Complementary or photographic-negative colors | Ensure `INVOFF (0x20)`, not `INVON (0x21)` |
| Static strip at left/top | Remove X/Y offsets; use origin `(0, 0)` |
| Image rotated or mirrored | Restore `MADCTL = 0x68` |
| Random or unstable pixels | Reduce SPI clock, verify ground and reset timing |
| Backlight on but no image | Check CS=15, SCLK=16, MOSI=17, D/C=7, RESET=18 |
| Flash reports ESP32/S3 mismatch | Delete stale build state and confirm `IDF_TARGET=esp32s3` |

For stale target state:

```sh
rm -rf build sdkconfig
idf.py set-target esp32s3
idf.py build
```

The repository already pins `esp32s3` in `CMakeLists.txt`; the cleanup is only
needed when reusing a build directory generated for another chip.

