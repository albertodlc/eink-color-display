# Image pipeline: PNG/JPG -> GDEP073E01 4 bpp

## Native format

The GDEP073E01 is an 800x480 panel and the controller expects **4 bpp** data:
two pixels per byte, **192,000 bytes** for a full frame.

```
byte = (left_pixel_index << 4) | right_pixel_index
```

Rows are sent top to bottom, left to right. The GxEPD2 driver converts the
palette indices below to the controller's native codes.

| Color | GxEPD2 index | Native nibble | Image2LCD RGB332 |
| --- | --- | --- | --- |
| Black | 0 | 0x0 | 0x00 |
| White | 1 | 0x1 | 0xFF |
| Green | 2 | 0x6 | 0x1C |
| Blue | 3 | 0x5 | 0x03 |
| Red | 4 | 0x3 | 0xE0 |
| Yellow | 5 | 0x2 | 0xFC |
| Orange | 6 | 0x4 | not used by this panel |

The 7.3" panel does not render the orange code; use indices 0-5 only.

## Official Good Display workflow

The attached Good Display guide uses **Image2LCD**:

1. Create an **800x480** image in BMP or JPG using the six native colors.
2. Open it in Image2LCD.
3. Output file type: **C array (*.c)**.
4. Scan mode: **horizontal scan**.
5. BitsPixel: **256 colors**.
6. Max Width/Height: **800 / 480**, then confirm.
7. Leave the five checkboxes indicated in the guide unchecked.
8. "Reverse color": enable only if you need to invert the image.
9. Save the `.c` and add it to the firmware.

Image2LCD's 256-color output is 8 bpp (384,000 bytes, one byte per pixel) using
RGB332 marker bytes. The official demo converts those markers to 4 bpp before
sending them. This project's converter does the same conversion and can also
read the Image2LCD `.c`/`.bin` directly.

## Using the converter

Requirements: Python 3 and Pillow.

```bash
python3 -m pip install -r requirements.txt
```

Convert a photo or graphic:

```bash
python3 tools/convert_image.py assets/foto.png -o src/foto.h --dither
```

Convert an Image2LCD `.c` or raw 8 bpp `.bin`:

```bash
python3 tools/convert_image.py IMAGE.c -o src/imagen.h
python3 tools/convert_image.py IMAGE.bin --format bin
```

Useful options:

| Option | Meaning |
| --- | --- |
| `--format c` / `--format bin` | C header or raw binary. |
| `--dither` | Floyd-Steinberg dithering (better for photos). |
| `--fit contain` | Keep aspect ratio and pad with white (default). |
| `--fit cover` | Fill the frame and crop the excess. |
| `--fit stretch` | Force 800x480. |
| `--palette panel` | Approximate measured Spectra 6 colors (default). |
| `--palette ideal` | Pure RGB primaries. |
| `--preview out.png` | Save the quantized image for inspection. |

The generated header contains:

```cpp
static const uint8_t foto_800x480[] PROGMEM = { ... };
static const uint16_t foto_800x480_width = 800;
static const uint16_t foto_800x480_height = 480;
```

If you feed an already packed 4 bpp `.bin`, it must use the GxEPD2 indices above.
An 8 bpp `.bin` or `.c` from Image2LCD is detected automatically.

## Displaying the image

```cpp
#include "foto.h"

// Full-screen write straight from flash, followed by a refresh.
display.drawNative(foto_800x480, nullptr, 0, 0, 800, 480,
                   /* invert = */ false,
                   /* mirror_y = */ false,
                   /* pgm = */ true);
```

`drawNative()` bypasses the paged drawing buffer and streams the 192 KB array
from flash. It is the lowest-RAM option for a full-screen image.

## Memory notes

- The 4 bpp full frame is 192,000 bytes. A C array of that size lives in flash,
  not RAM, when declared `PROGMEM`.
- The default 4 MB flash partition has room for the application plus one or two
  full-frame images. If you need more, use a larger partition table, the onboard
  W25Q16 flash, or an SD card.
- The paged drawing API (`firstPage()` / `nextPage()`) is still the best choice
  for text and vector screens because it only needs a 64 KB page buffer.

## Color accuracy

Spectra 6 has a limited, non-sRGB gamut. The `panel` palette uses approximate
measured colors; adjust the values in `tools/convert_image.py` if you measure
your panel. Dithering improves gradients but can look grainy at close distance.
