Board variant for the Seeed XIAO nRF52840, copied unchanged from
Seeed-Studio/Adafruit_nRF52_Arduino (`variants/Seeed_XIAO_nRF52840`, core 1.1.13).

PlatformIO's nordicnrf52 platform has no XIAO definition and its Adafruit
core package has no XIAO variant, so the pin map lives here and
`boards/xiao_nrf52840.json` points `build.variants_dir` at this directory.
