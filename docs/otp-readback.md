# ED2208 / SPD1657A controller research and OTP readback

## TL;DR

- There is **no public ED2208-GCA controller firmware image or SDK**. The
  ED2208-GCA name is the module/Seeed designation; the silicon on this panel
  reports **Solomon Systech SPD1657A** (REV `0x70` returns `01 01 04`).
- The 96 KB on-chip OTP is a **waveform table**, not executable firmware. It is
  factory-programmed (OTP program mode needs VPP 7.5 V) and cannot be rewritten
  in the field.
- The **module** datasheet says the serial interface is write-only, but the
  controller manual documents read commands and the controller **does** drive
  the shared SDI line. On the ESP32-L + DESPI-C73 kit, GPIO23 (MOSI) can be
  turned into an input and used as the read data line.
- Read commands used: `REV 0x70`, `FLG 0x71`, `ROTP 0x92`, `RSRAM 0x93`,
  `DSP 0x11`.
- A 96 KB raw dump was extracted. On this unit only the first five 400-byte OTP
  pages are byte-reproducible; later pages show single-bit errors even at
  4-10 MHz. The public gist and a different MCU/SPI setup are the references.

## Datasheet

The 44-page **SPD1657A Rev 1.0** manual (Aug 2021, marked "Advance
Information") is available from the public Google Drive link posted by the
SPD1657A author:

<https://drive.google.com/file/d/1U1hq4dhr821CVItF9kOPBJRxhkNMd6Rb/view>

A local copy is at [`SPD1657A-1.0.pdf`](SPD1657A-1.0.pdf).

What it confirms:

- 800 source x 600 gate active-matrix color EPD driver with controller. The
  7.3" panel uses 800x480 of that range.
- Embedded OTP for waveform settings; `ROTP 0x92` reads it (dummy byte first).
- `PGM 0x90` / `APG 0x91` OTP program mode uses VPP 7.25-7.75 V. This is a
  factory/lab process, not a field update; the module does not expose VPP on the
  DESPI-C73 header.
- PSR B[7] `LUT_EN`: `0` = auto-load LUT from OTP on reset (default); `1` = no
  OTP auto-load, analog settings follow the MCU. The stock PSR `0x69` has
  `LUT_EN=0`; the Tosone PSR `0xE9` sets it to 1, which is why that recipe needs
  an MCU-side LUT.
- PLL `0x30` frame-rate table: `000`=25 Hz, `010`=43 Hz default, `011`=50 Hz,
  `111`=100 Hz. The vendor sends `0x08`; the project log calls `0x08` canonical
  and `0x07` a 2x overclock. The manual is Advance Information and incomplete.
- Partial window `0x83` uses HRST/HRED/VRST/VRED and PMODE; gate scan covers
  both inside and outside the window.
- 4-wire SPI pins: SCL, SI0 (MISO), SI1 (MOSI), CSB. The module exposes a single
  SDI line; in practice it carries both write data and readback.

## What is and is not available

| Item | Availability |
| --- | --- |
| ESP32 application firmware | Yes: this repo + `docs/ESP32-GDEP073E01.zip` |
| SPD1657A controller manual | Yes: public Google Drive link + local PDF |
| Host drivers (GxEPD2, Zephyr, epdsi, FreeInk) | Yes, open source |
| E Ink THOR/ICE/Luvia host-board firmware | Vendor support files; not the controller |
| ED2208/SPD1657A mask-ROM firmware | No public image, no JTAG/SWD |
| On-chip OTP waveform | Readable over SPI; factory-programmed, not field-writable |
| External waveform file (`.wbf`) | Some E Ink kits support it; this panel uses OTP |

The controller is a COG ASIC. There is no user-updatable controller firmware to
modify. You can change the host init sequence, send commands, and read the OTP,
but the waveform engine is fixed.

## Identifying the controller

`REV (0x70)` returns three bytes; on this panel:

```text
REV: 01 01 04
```

The final `0x04` is the SPD1657A family identifier. The widely cited gist calls
the part SPD1656; the SPD1657A project log reports that the two are only about
85% command-compatible. `0xCD` is not the version register on this unit
(it returned `0x00`).

## Readback method

The DESPI-C73 exposes only `SDI`, not a separate MISO. The panel's SI0 is
bidirectional. The sequence used here:

1. Send the read command with `DC=0`, `CS=0`.
2. Release SDI (GPIO23) as an input.
3. Clock SCK (GPIO18) and sample GPIO23.
4. `ROTP 0x92`: read 96,001 bytes; the first byte is a dummy.
5. Send `DSP 0x11` to stop, then hibernate the panel.

Firmware flags (default off):

```bash
# Read REV/FLG and a short ROTP sample
PLATFORMIO_BUILD_FLAGS="-DEPD_READ_TEST=1" pio run -t upload

# Dump 96,000 OTP bytes over serial
PLATFORMIO_BUILD_FLAGS="-DEPD_DUMP_OTP=1" pio run -t upload
python3 tools/dump_otp.py --port /dev/ttyUSB0 -o /tmp/otp.bin
```

The dump build skips the air-traffic refresh, reads the OTP, hibernates, and
stays read-only. `tools/dump_otp.py` captures the raw binary after the
`[otp] BEGIN 96000` marker.

## Public dump implementations

Searched GitHub (repository, issue and commit search), GitLab, and the cloned
GDEP073E01 projects. No public SPD1657A OTP-dump tool was found.

- GitHub repo search: **0** results for `SPD1657A`, **0** for `ED2208`, **7** for
  `GDEP073E01`. The seven are display drivers/frames, not dump tools:
  `xandronak/gdep073e01`, `DenSyo77/GDEP073E01WebServer`,
  `martinpl/canvas-firmware`, `multicatch/esp32_eink_picture_frame`,
  `iceton/good-display-epaper-frame`, `mamekudz/microPhotoFrame`,
  `Gronner/gdep073e01-rs`.
- `ts-manuel/Understanding-ACeP-Tecnology`: the best public methodology
  reference. It extracts factory LUTs from an **external SPI flash** on a
  5.65" SPD1656 panel, using command `0x65` to bypass the controller's flash
  master and read a W25X20CL. That is not the SPD1657A internal OTP, but it
  documents the LUT/flash side of the family.
- `codeart1st` gist: the public 96 KB OTP dump data, but no capture process.
- roob123's Hackaday project: describes the `0x92 ROTP` read and the Zephyr UART
  buffer rebuild (4096 bytes, 921600 baud) used for a reliable 96 KB read. The
  source code is not published.
- GxEPD2 `enableRead()` / `_readData()`: the generic read primitives used here.
  The Zephyr ED2208 driver, epdsi and FreeInk do not expose an OTP dump path.
- This repo: `tools/dump_otp.py` plus `-DEPD_DUMP_OTP=1` is the reproducible
  ESP32 capture process. On this unit only the first five pages verify.

## Results on this unit

- `REV`: `01 01 04`
- `FLG`: `02` or `04` depending on power state
- Raw dump: 96,000 bytes, captured with the dump build and kept out of the
  repository.
- Verified prefix: the first block of OTP pages 0-4, 2,000 bytes; regenerate it
  with `tools/dump_otp.py` when needed.
- The stable prefix is identical across independent dumps.
- Beyond page 4, roughly 15-25% of bytes differ between runs by single bits.
  The pattern is periodic: within a 1,200-byte page the first 400-byte block is
  stable and the next two are marginal. The two marginal blocks are not needed
  for the confirmed waveform prefix.
- A public 96 KB dump exists in the codeart1st gist. The SPD1657A project log
  reports a reliable 96 KB read after rebuilding a Zephyr UART buffer and using
  an STM32U585 hardware-SPI setup; that code is not published.

The verified prefix is the useful artifact here. Treat the raw dump as
**partially unreliable** beyond the first five pages; do not use it to infer
waveform bytes without cross-checking the public gist or repeating the read on a
different MCU.

## Init/refresh experiments

These were tried on the same panel and are documented so they are not repeated
blindly:

- **Vendor `EPD_init_fast()`**: panel BUSY 30.5 s versus 26.2 s for the stock
  GxEPD2 init. Slower; keep the stock sequence. Available behind
  `-DEPD_FAST_INIT=1`.
- **"Tosone's Law" (PSR `0x00` first after reset)**: simplified variants using
  PSR `0x5F,0xE9` and `0x5F,0x69` both hung at the 45 s BUSY timeout. The manual
  explains part of this: PSR B[7] `LUT_EN=1` (the `0xE9` value) disables OTP LUT
  auto-load and expects an MCU-supplied LUT; without one the refresh can hang.
  The `0x69` variant has `LUT_EN=0`, but it also hung, so additional init steps
  (PWRR/LUT staging/other commands) are still missing from the public recipe.
  The author reports 14-15 s DRF with the complete Tosone-compliant init; not
  reproduced here. Code is available behind
  `-DEPD_TOSONE_INIT=1` for reference, but it is not the default.
- **Partial refresh**: the author reports ~1.5 s window updates with
  `PTLW 0x83`, `CDI 0x50=0xFF`, `DRF 0x12=0x01`, at the cost of washed-out
  color and window-edge artifacts. Our 100x60 vendor and DCVCOM tests each took
  26.0 s on this unit, so the fast result was not reproduced; see
  [refresh-speed.md](refresh-speed.md). GxEPD2's partial support uses
  `DRF=0x00`.
- **FreeInk's interrupted-refresh monochrome mode**: cuts a refresh at ~340 ms
  for fast black/yellow rendering; requires periodic complete waveforms for DC
  balance. Research-grade, not full color.

## Safety

- Only read commands were used here. Do not write unknown VCOM, booster, PLL or
  LUT registers without vendor guidance; a wrong value can damage the panel.
- OTP is one-time programmable. It cannot be changed by SPI.
- Keep the panel powered while reading and call `hibernate()` afterwards.
- The DESPI-C73 3.3 V rail is mandatory; 5 V on the panel logic will damage it.

## References

- codeart1st OTP gist: <https://gist.github.com/codeart1st/dfa41e6426388dc929c719c4913297e3>
- SPD1657A bring-up project log: <https://hackaday.io/project/205770>
- Modos Glider / Caster (open FPGA EPD controller, waveform format):
  <https://github.com/Modos-Labs/Glider>
- GxEPD2 GDEP073E01 driver:
  <https://github.com/ZinggJM/GxEPD2>
- Zephyr ED2208-GCA driver:
  <https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/display/display_ed2208_gca.c>
- epdsi ED2208 driver:
  <https://docs.rs/epdsi/latest/epdsi/controllers/ed2208/index.html>
- Good Display GDEP073E01 datasheet (local): `docs/GDEP073E01-1.0.pdf`
