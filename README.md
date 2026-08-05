# ClavisMacro RGB Matrix v1

Primera versión del motor RGB personalizado para ZMK/Zephyr. Usa directamente
`led_strip_update_rgb()` mediante un `k_work_delayable`, igual que la prueba
roja/azul que sí funcionó de manera continua.

## Qué incluye

- Mapa físico normalizado de 10 LEDs (`x`, `y`, `key_id`, zona).
- Static.
- Breathe.
- Wave X.
- Wave Y.
- Wave Diagonal.
- Rainbow Spatial.
- Chase.
- Index Scan para identificar el orden físico de la cadena.
- Brillo, hue, saturación, velocidad, dirección y máscara de LEDs.
- Persistencia en `clavis/rgb/state` mediante Zephyr Settings.
- Behavior `&cl_rgb` compatible con los comandos `RGB_*` normales de ZMK.
- EC0 en Base cambia efectos; EC0 en RGB cambia brillo.
- API preparada para que después la pantalla y el programa de PC lean/cambien el estado.

## Archivos que se reemplazan

Copia estos archivos a:

`config/boards/shields/clavismacroxiao/`

- `clavis_led_map.h`
- `clavis_rgb_engine.h`
- `clavis_rgb_engine.c`
- `behavior_clavis_rgb.c`
- `CMakeLists.txt`
- `clavismacroxiao.keymap`

El archivo:

`dts/bindings/behaviors/zmk,behavior-clavis-rgb.yaml`

debe ir en la misma ruta desde la raíz del repositorio. El repositorio ya se
pasa como `ZMK_EXTRA_MODULES`, por lo que esa carpeta de bindings pertenece al
módulo, no dentro del shield.

## Overlay RGB

Se incluye el overlay actual de REV 1 como referencia/reemplazo:

`config/boards/shields/clavismacroxiao/boards/xiao_ble_nrf52840_zmk.overlay`

Para REV 2, las dos apariciones de:

```dts
NRF_PSEL(SPIM_MOSI, 1, 5)
```

cambian a:

```dts
NRF_PSEL(SPIM_MOSI, 0, 4)
```

El motor C no cambia entre REV 1 y REV 2.

## `.conf` para la primera prueba

Conserva el `.conf` actual sin pantalla durante esta primera prueba y deja la
sección RGB así:

```conf
CONFIG_SPI=y
CONFIG_LED_STRIP=y
CONFIG_WS2812_STRIP_SPI=y
CONFIG_SETTINGS=y
CONFIG_ZMK_RGB_UNDERGLOW=n
```

Elimina o comenta las demás opciones `CONFIG_ZMK_RGB_UNDERGLOW_*`. No pueden
estar activos el worker de ZMK y el motor Clavis al mismo tiempo.

También quita `snippet: zmk-usb-logging` del `build.yaml` para esta prueba.

## Archivo viejo de diagnóstico

`clavis_rgb_test.c` puede quedarse en la carpeta, pero el nuevo `CMakeLists.txt`
no lo compila. También puede borrarse para evitar confusión.

## Controles

### EC0 en Base

- Giro horario/antihorario: siguiente/anterior efecto.
- Presión: entra o sale de la capa RGB.

### EC0 en RGB

- Giro: brillo ±10%.
- Presión: vuelve a Base.

### EC1

- Giro: volumen.
- Presión: mute.

## Orden de efectos

1. Static
2. Breathe
3. Wave X
4. Wave Y
5. Wave Diagonal
6. Rainbow Spatial
7. Chase
8. Index Scan

El estado se guarda. Si el primer arranque muestra otro efecto, gira EC0 hasta
recorrer los ocho.

## Mapa físico provisional

Se asume que los índices 0–8 coinciden con las nueve teclas Hall en orden por
filas y que el índice 9 es el LED del encoder/auxiliar:

```text
0  1  2
3  4  5   9
6  7  8
```

`Index Scan` enciende un LED a la vez. Si el orden eléctrico real no coincide,
solo se corrige `clavis_led_map.h`; ningún efecto ni behavior necesita cambios.

## Pantalla

No se modificó `clavis_status_screen.c`. El `CMakeLists.txt` conserva su bloque
condicional. Después de validar el motor RGB, se restauran las opciones de
pantalla del `.conf` bueno y se conecta la UI a `clavis_rgb_get_state()`.
