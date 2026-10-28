# Refresh-speed research: GDEP073E01 / SPD1657A

Research date: 2026-10-27. Target: Good Display GDEP073E01, 7.3" 800x480
Spectra 6, Solomon Systech SPD1657A controller.

## Executive summary

- **Full-color refresh is panel/OTP-bound.** Stock full refresh is 26-27 s on
  this unit; independent measurements on a reTerminal E1002 report 28-32 s.
  There is no public software switch that makes a full-color Spectra 6 refresh
  dramatically faster.
- **Small-window partial refresh is widely claimed to be the speed win, but it
  did not reproduce on this unit.** The official Good Display partial example
  was measured at **2.1 s for a 100x60 block** on a reTerminal E1002, and a
  detailed SPD1657A project reports **~1.5 s** windows. On this GDEP073E01 unit,
  both the vendor sequence and the DCVCOM/CDI `0xFF` variant took **26.0 s** —
  the same as a full refresh. Partial refresh avoids the full-screen flash, but
  not the panel BUSY time here.
- **ESPHome and our local test agree on ~26 s** for a partial window. Treat the
  2.1 s / 1.5 s claims as panel-batch/geometry/measurement-dependent until
  reproduced on the exact unit.
- **Do not expect a free full-color 14 s refresh.** A reported 14-15 s full
  refresh ("Tosone's Law": PSR first after reset) is not reproducible from the
  public information; our simplified variants hung at the BUSY timeout. The
  manual explains part of it (PSR `LUT_EN`), but the full recipe is missing.
- **Avoid overclocking/custom-LUT hacks.** PLL overclocking produces color
  artifacts; custom per-color LUT writes are not supported on this SPD1657A
  revision; OTP is factory-programmed and cannot be rewritten in the field.

## Measured/claimed timings

| Method | Time | Source / notes |
| --- | --- | --- |
| Full refresh, stock GxEPD2 | **26.0-26.2 s** | This unit, panel BUSY |
| Full refresh, GxEPD2 | 32.4 s | Hackster, reTerminal E1002, 21 °C |
| Full refresh, Good Display | 28.1 s | Hackster, same setup |
| Full refresh, Waveshare | 30.6 s | Hackster, same setup |
| Partial 100x60, vendor sequence | **26.0 s** | **This unit; not faster** |
| Partial 100x60, DCVCOM/CDI 0xFF/DRF 0x01 | **26.0 s** | **This unit; not faster** |
| Partial 100x60, Good Display sample | 2.1 s (claimed) | Hackster; not reproduced here |
| Partial windows, SPD1657A reverse engineering | ~1.5 s (claimed) | TANG.log, fix3; not reproduced here |
| Partial window, GxEPD2/ESPHome | ~26 s | ESPHome PR #20944, matches this unit |
| Vendor `EPD_init_fast` full refresh | 30.5 s | This unit; slower than stock |
| Tosone PSR-first full refresh | 14-15 s (claimed) | Hackaday project log; not reproduced |
| Interrupted-refresh monochrome | ~340 ms | FreeInk M5 PaperColor; not full color |
| Gallery 3 full color | ~1.5 s | Different panel technology |
| Kaleido 3 color | ~1-2 s | Color-filter e-paper, lower saturation |

## Local validation (2026-10-27)

We implemented the vendor `0x83` sequence in
`epd::Gdep073e01LongTimeout::partialWindowSolid()` and ran it on this unit after
a full refresh:

| Sequence | Window | Panel BUSY |
| --- | --- | --- |
| Vendor: prefix `0x00`, `CDI 0x3F`, `DRF 0x00` | 100x60 at (100,300) | **25,974 ms** |
| DCVCOM: no prefix, `CDI 0xFF`, `DRF 0x01` | 100x60 at (300,300) | **25,975 ms** |

Both are the same as a full refresh (25,975 ms vs 26,023 ms). The window data
was sent and the blocks appeared, but the controller ran the full OTP waveform.
Partial refresh on this unit therefore saves the full-screen flash/flicker, not
time. The 2.1 s/1.5 s claims need a different panel batch, the fix3 geometry, or
a measurement that excludes the BUSY wait; they are not a safe basis for a
“fast UI” design on this unit.

## Partial-refresh sequences

The official Good Display `GDEP073E01_Partial` example (ported in
`melastmohican/reTerminal-E1002-arduino-examples`) does:

1. `0x83` partial window: `x`, `x_end`, `y`, `y_end` (inclusive), then `0x01`
   (`PMODE=1`).
2. `0x10` data start, followed by a `0x00` byte and the window's 4 bpp pixels.
3. `CDI 0x50 = 0x3F`.
4. `DRF 0x12 = 0x00`, then wait for BUSY.

Hackster reported that sequence at **2.1 s** for a 100x60 block. Our local test
of the same sequence on this unit took **26.0 s** (see “Local validation”), so
the fast result is not universal. The vendor ReadMe notes the same trade-off:
partial windows use a gentler waveform, so colors are lighter/pastel;
periodically run a full refresh to restore contrast and clear ghosting.

The TANG.log project reports **~1.5 s** windows with:

- `0x83` partial window, `CDI 0x50 = 0xFF` (border HiZ), `DRF 0x12 = 0x01`,
- a "fix3" window protocol: declare `H+2` rows, send a dummy row plus `H`
  real rows, never issue a raw reset before partial updates,
- 2-4 rows of same-color padding around dynamic content.

The TANG project also documents a hardware quirk: on raw reset the controller
uses the first ~98.4 KB of frame SRAM as an OTP staging buffer, which corrupts
the first 247 rows; partial windows whose bottom edge falls in the upper half
can show an edge line. The fix3 protocol avoids it.

### The `DRF` byte is not a partial-refresh flag

The SPD1657A manual defines `DRF 0x12` data bit `A[0]` as **AC/DC VCOM
selection**:

- `0` = ACVCOM, FPL_VCOM follows LUTC during update (default)
- `1` = DCVCOM, FPL_VCOM stays at VCOMDC during update

So the TANG `DRF=0x01` is DCVCOM, not a "fast mode" bit. It changes the common
electrode behavior and is likely part of why that path shows washed-out color /
DC-balance concerns. The vendor sample uses `DRF=0x00`; the claimed 2.1 s
result is not reproduced here.

### Why partial may still take ~26 s

The ESPHome PR implements `0x83` + `0x10` (no prefix) + `CDI 0xFF` + `DRF 0x00`
and measured ~26 s. Differences between the sequences:

- `CDI 0x3F` (vendor) vs `0xFF` (ESPHome) — border driven vs HiZ.
- A `0x00` byte after `0x10` in the vendor sample (the ESPHome PR drops it
  because it shifts the pixel stream by 2 px).
- Window geometry and pane height; the claimed fast result is for a **100x60**
  block.

The controller documentation does not state which combination selects the
short partial waveform. Our local test of both the vendor sequence and the
DCVCOM variant produced 26 s each, so on this unit there is no fast-partial
mode to select. Treat the 2.1 s / 1.5 s claims as panel/geometry/measurement
dependent and verify on the exact panel before designing a UI around them.

## Full-refresh attempts

### Stock init: 26 s

GxEPD2 sends `0xAA`, PWRR, PSR `0x5F,0x69`, booster, PLL `0x08`, ... The panel
BUSY wait is 26.0-26.2 s on this unit.

### Vendor `EPD_init_fast`: slower

The Good Display sample's alternate sequence (PLL `0x02`, different PWRR/BTST/
VDCS) measured **30.5 s** panel BUSY. It is behind `-DEPD_FAST_INIT=1` and is
not recommended.

### Tosone's Law: plausible but incomplete

The SPD1657A project log reports that writing PSR (`0x00`) first after reset,
before PWRR/BTST/PLL, cuts DRF from 28-46 s to **14-15 s**. Our simplified
PSR-first variants with `0x5F,0xE9` and `0x5F,0x69` both hung at the 45 s BUSY
timeout.

The manual explains part of the failure: PSR `B[7]` is `LUT_EN`.

- `0` = auto-load LUT from OTP on reset (default; stock `0x69`)
- `1` = no OTP auto-load; analog settings follow the MCU (Tosone `0xE9`)

The `0xE9` value disables OTP LUT auto-load and expects an MCU-side LUT. Without
one, the refresh can hang. The `0x69` variant also hung, so additional init
steps are missing. Do not guess the full recipe on the only panel; get the
complete sequence or vendor support first.

### PLL overclock: artifacts

The PLL register supports 25-100 Hz frame rates. Running at a higher frame rate
shortens the waveform but under-drives the pigments; the SPD1657A project log
found its "new palette" results were overclock artifacts. Not a production
speedup.

### Custom LUT: not available on this revision

The SPD1657A Rev 1.0 (IC version `0x04`) silently ignores the SPD1656 LUT-write
unlock (`0x33 -> 0xAB -> 0xCD`); `0x21-0x28` alias one SRAM buffer; custom
per-color waveforms are not possible. OTP is factory-programmed with VPP 7.5 V;
it is not a field update.

## Host-side improvements

These do not change panel physics but reduce total time-to-image:

- Merge screens into one refresh (done in this project: ~60 s -> ~28 s).
- Draw only the current page band (done: draw time 2.3 s -> 0.9 s).
- Pre-render images in flash/PSRAM and stream one frame (`drawNative`); avoids
  paged redraw overhead and SD seeks.
- Transfer at 10-20 MHz; the 192 KB transfer is only ~0.1-1 s, so the gain is
  small compared with the 26 s panel wait.
- Do not re-init/hibernate between a batch of updates; save the ~0.2-0.5 s
  power-on/init per update.
- Update only when content changes; respect the 180 s vendor interval.

## Hardware alternatives

If the product needs fast full-color updates, this panel is the wrong
technology:

- **E Ink Gallery 3**: ~1.5 s full color, ~350 ms pen input; different
  waveform/controller, much faster.
- **E Ink Kaleido 3**: ~1-2 s color, color-filter based, lower saturation than
  Spectra 6.
- **Monochrome / 3-color e-paper**: true fast partial refresh (0.3-3 s) for
  text/status, but no six-color photos.
- **LCD/OLED**: instant, but not reflective/bistable.
- **FPGA direct-drive of a raw panel** (Modos Glider/Caster): 60-75 Hz on
  controller-less panels, but this integrated-controller module cannot be
  driven that way.

## Recommendations for this project

1. **Use full refresh for photos/color.** 26-28 s is the honest cost.
2. **Partial refresh is not a speed win on this unit.** It took 26.0 s for both
   the vendor and DCVCOM sequences. Use it only to avoid the full-screen flash,
   and keep periodic full refreshes for ghosting/contrast.
3. **Do not use `DRF=0x01`/DCVCOM unless you accept the color/DC trade-off.**
   The manual defines it as DCVCOM, not a speed flag.
4. **Do not pursue Tosone/full-color 14 s without the complete recipe.** The
   simplified sequence hangs; the manual shows why `LUT_EN` matters.
5. **If full-color speed is a product requirement, change panel technology**
   (Gallery 3/Kaleido 3) or use a raw-panel FPGA design; do not fight the
   integrated SPD1657A waveform.

## Sources

- Hackster, "Driving the reTerminal E1002 7.3\" Color E-Paper with Arduino"
  (full/partial measured timings, vendor partial code link):
  <https://www.hackster.io/melastmohican/driving-the-reterminal-e1002-7-3-color-e-paper-with-arduino-21070f>
- Good Display partial sample port:
  <https://github.com/melastmohican/reTerminal-E1002-arduino-examples>
- ESPHome PR #20944, partial-window implementation and ~26 s note:
  <https://github.com/esphome/esphome/pull/20944>
- SPD1657A bring-up retrospective (Tosone's Law, recipes, 1.5 s window,
  PSR/LUT notes): <https://hackaday.io/project/205770>
- TANG.log, "Modos Glider and my six-color screen" (fix3, `y≈248`, `0x93`
  readback, 1.5 s partial):
  <https://www.tang01.cc/posts/modos-glider-reading-notes/>
- SPD1657A Rev 1.0 manual (local): [SPD1657A-1.0.pdf](SPD1657A-1.0.pdf)
- FreeInk SDK interrupted-refresh mode:
  <https://github.com/Free-Ink/freeink-sdk>
- GxEPD2 GDEP073E01 driver:
  <https://github.com/ZinggJM/GxEPD2>
- Seeed forum partial-refresh fading report:
  <https://forum.seeedstudio.com/t/ee04-and-spectra-6-7-3-partial-refresh/295260>
- Core Electronics real-world refresh discussion:
  <https://forum.core-electronics.com.au/t/7-3-6-color-epd-refresh-rate-ws-27875/23398>
