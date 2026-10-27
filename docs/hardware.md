# Hardware: ESP32-L(C73) + GDEP073E01

## Kit contents

| Part | Description |
| --- | --- |
| **ESP32-L** | Main board, ESP32-WROOM-32D, 4 MB flash, USB-C + CH340, 5 V input, reset button, LED, font chip GT30L32S4W, flash chip W25Q16, current-measurement points TEST1/TEST2, jumper P3. |
| **DESPI-C73** | Adapter board with a 50-pin FPC connector for 4" and 7.3" E6 panels. Plugs into the ESP32-L `P5` header. |
| **GDEP073E01** | 7.3", 800x480, E Ink Spectra 6, SPI, 50-pin FPC, 15-22 s full refresh. |

## Panel specifications

| Parameter | Value |
| --- | --- |
| Model | GDEP073E01 |
| Technology | E Ink Spectra 6 (black, white, red, yellow, blue, green) |
| Resolution | 800 x 480 |
| Active area | 160 x 96 mm |
| Outline | 170.2 x 111.2 x 0.91 mm |
| Interface | SPI, 50-pin FPC |
| Full refresh | 15-22 s |
| Operating temperature | 0-50 °C (panel) |
| Logic / supply | 3.3 V |

The ESP32-L board itself is rated -20 to 70 °C, but the panel limits the system to 0-50 °C.

## ESP32-L P5 / DESPI-C73 P2 pinout

| P5 pin | DESPI-C73 P2 | Signal | ESP32 GPIO |
| --- | --- | --- | --- |
| 1 | 1 | 3.3V_S2 | 3.3 V |
| 2 | 2 | GND | GND |
| 3 | 3 | SDI / SPI_MOSI | 23 |
| 4 | 4 | SCLK / SPI_SCK | 18 |
| 5 | 5 | CS | 27 |
| 6 | 6 | D/C | 14 |
| 7 | 7 | RES | 12 |
| 8 | 8 | BUSY | 13 |

`MISO` (GPIO19) is not connected to the panel.

The mapping was taken from:

- `ESP32-L_SCH.pdf` (U5 ESP32 module and P5 connector)
- `DESPI-C73-SCH.pdf` (P2 host connector)

## Assembly

1. Plug **DESPI-C73** into the **P5** header of the ESP32-L.
2. Open the FPC latch, insert the panel's 50-pin FPC with the **gold contacts facing down**, then close the latch.
   The Good Display manual says: "cara especular hacia abajo, contactos dorados hacia abajo"; follow the figure in the manual for your unit.
3. Make sure the **P3 jumper is installed**. P3 connects power to the adapter and panel; without it the display stays blank.
4. Connect a **USB-C data cable** (not charge-only) to a 5 V source and install the **CH340** driver if needed.
5. Turn the power switch to **ON**.

## Power and current measurement

- Input: 5 V through USB-C.
- Main board current: switch OFF, connect an ammeter across **TEST1**.
- Adapter/panel current: switch ON, remove **P3**, connect an ammeter across **TEST2**, then reinstall P3.
- Panel refresh current spikes are normal; use a stable 5 V supply.

## Refresh timing and busy timeout

The GDEP073E01 full refresh takes about **26-27 s** on this kit (the datasheet
range is 15-22 s, but the measured value is longer). GxEPD2's default busy
timeout for this driver is 20 s, so the stock driver prints `Busy Timeout!` and
sends power-off while the panel is still refreshing.

`include/epd_config.h` derives a small driver subclass with a **45 s** busy
timeout. This keeps the driver waiting until BUSY is released and makes the
power-off sequence safe. The serial log should show a refresh around 26 s and no
`Busy Timeout!` message.

## Notes

- All panel signals are **3.3 V**. Do not apply 5 V to SPI, CS, DC, RST or BUSY.
- GPIO12 is a boot strapping pin (MTDI). The ESP32-L is designed with GPIO12 as panel RES; do not add an external pull-up to this net.
- The panel keeps the last image with no power. After an update call `display.hibernate()` and, for battery use, enter deep sleep.
- **Do not insert or remove the panel FPC while the board is powered.**
- A full refresh flashes/flickers while it clears ghosting. This is normal.
- Always leave the panel in sleep mode after a refresh; this protects its lifespan.
- Re-initialize the panel for every full-screen update.
- The kit also supports the **4" GDEP040E01** panel, but its driver is not the same. This project targets the 7.3" GDEP073E01.

## Handling and lifetime

From the Good Display usage guidelines:

- Multi-color e-paper does not operate below **0 °C**; this panel is rated 0-50 °C.
- Do not press the driver IC or the FPC-to-glass bonding area. Hold the panel by
  the glass, never lift it by the FPC, and do not bend the FPC upward.
- **Power off before inserting or removing the FPC.**
- Use a supply with enough current capacity. A weak battery can damage the panel IC.
- Allow at least **180 s after a set of updates** before starting the next set.
  Update only when the content changes.
- Use deep sleep/hibernation between updates. If the battery is running low,
  update the panel to full white before cutoff.
- Store at approximately 23 ± 3 °C and 55 ± 10 %RH.

## Local reference documents

The `docs/` folder contains the vendor material used to build this scaffold:

- `ESP32-GDEP073E01.zip` - official ESP32 sample code and `image.h`.
- `STM32-GDEP073E01.zip` - official STM32 sample code.
- `GDEP073E01-1.0.pdf` - panel datasheet.
- `GDEP073E01 Picture Production and Bitmap Conversion00.pdf` - Image2LCD workflow.
- `Image2LCD Software Bitmap Conversion Instructions V1.0.pdf` - Image2LCD settings.
- `ePaper Display Usage Guidelines.pdf` - handling and lifetime recommendations.

The official ESP32 sample uses `A14/A15/A16/A17` for BUSY/RES/DC/CS. On the LOLIN32/ESP32 mapping used by the kit these are **GPIO13/GPIO12/GPIO14/GPIO27**, matching the table above.


## References

- ESP32-L product: <https://www.good-display.com/product/338.html>
- GDEP073E01 product: <https://www.good-display.com/product/533.html>
- ESP32-L(C73) kit: <https://www.buyepaper.com/products/development-kit-epaper-board-esp32-lc73>
- ESP32-L schematic: <https://v4.cecdn.yun300.cn/100001_1909185147/ESP32-L_SCH.pdf>
- DESPI-C73 schematic: <https://v4.cecdn.yun300.cn/100001_1909185147/DESPI-C73-SCH-20251014.pdf>
- Panel picture/bitmap guide: <https://v4.cecdn.yun300.cn/100001_1909185148/GDEP073E01%20Picture%20Production%20and%20Bitmap%20Conversion00.pdf>
