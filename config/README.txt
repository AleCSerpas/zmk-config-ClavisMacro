ClavisMacro ZMK consolidated test
================================

Included changes:
- ST7789 rotated 180 degrees.
- Custom 240x240 LVGL screen.
- Dynamic Base/RGB layer label.
- Dynamic RGB brightness ring and selected hue.
- Dynamic battery ring hidden at a 0% reading.
- Static 36% volume placeholder.
- One-minute idle timeout.
- RGB and display blank on idle.
- EC0: RGB effects on Base, RGB brightness on RGB layer, press toggles layer.
- EC1: volume on both layers, press mutes.
- SPI3 SK6812 driver on D18/P1.05.

Copy the config folder over the config folder in your zmk-config repository.
Keep your existing build.yaml, west.yml, Kconfig.shield, Kconfig.defconfig,
and clavismacroxiao.zmk.yml files.
