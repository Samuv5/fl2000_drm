# fl2000 — driver DRM para adaptadores USB 3.0 Fresco Logic FL2000

Driver de kernel Linux para los adaptadores USB 3.0 a VGA / HDMI basados en el chip
**Fresco Logic FL2000DX** (USB ID `1d5c:2000`). El adaptador aparece como una tarjeta de video
más (`/dev/dri/cardN`), así que funciona con cualquier escritorio: GNOME y KDE en Wayland,
Cinnamon y otros en X11, y la consola de texto.

Probado con un adaptador Steren VGA+HDMI en CachyOS (kernel 7.2, clang) con GNOME Wayland a
1920x1080 @ 60 Hz. Compila también en kernels LTS 6.18 y en 7.0 con gcc.

## Instalación (CachyOS / Arch)

```sh
sudo pacman -S --needed base-devel dkms linux-cachyos-headers   # o los headers de tu kernel
git clone https://github.com/Samuv5/fl2000_drm
cd fl2000_drm
makepkg -si
```

El paquete `fl2000-dkms` recompila el módulo automáticamente en cada actualización de kernel
(detecta solo si el kernel se compiló con clang). Conecta el adaptador y listo; si tu sesión
gráfica ya estaba abierta, cierra sesión y vuelve a entrar para que el escritorio lo tome.

### Otras distribuciones / prueba sin instalar

```sh
make
sudo insmod fl2000.ko
```

## Uso y ajustes

- Resolución y posición: en la configuración de pantallas del escritorio.
- `max_bpp`: bits por pixel enviados por USB, `24` (predeterminado) o `16`. Con 16 se usa la
  mitad del ancho de banda, útil en puertos USB lentos o hubs:
  `echo 16 | sudo tee /sys/module/fl2000/parameters/max_bpp` (aplica en el siguiente cambio de
  modo) o `options fl2000 max_bpp=16` en `/etc/modprobe.d/fl2000.conf`.
- Mensajes de depuración: `sudo insmod fl2000.ko dyndbg=+p`.

## Limitaciones conocidas

- Necesita USB 3.0. En USB 2.0 solo caben resoluciones muy pequeñas.
- Si el adaptador no da acceso al EDID del monitor (como el Steren probado), se detecta el
  monitor por carga eléctrica en VGA y se ofrecen resoluciones estándar hasta 1920x1200.
- Adaptadores HDMI con chip ITE IT66121: soporte portado del driver original, sin probar en
  hardware. Los que convierten VGA a HDMI con otro chip funcionan como VGA.
- Sin audio por HDMI.
- GNOME no siempre toma el adaptador si se conecta con la sesión ya abierta (limitación de
  mutter): cierra sesión y vuelve a entrar.

## Diseño

Un solo módulo (`fl2000.ko`) siguiendo el patrón de los drivers USB del kernel (`udl`, `gud`):
GEM SHMEM con plano "shadow", PRIME para recibir la imagen de la GPU principal, sin vblank real.
La secuencia de configuración del chip y el protocolo de streaming siguen el
[driver oficial de Fresco Logic](https://github.com/FrescoLogic/FL2000).

| Archivo | Contenido |
|---|---|
| `fl2000_drv.c` | Dispositivo USB y DRM, endpoint de interrupciones, suspensión |
| `fl2000_modeset.c` | Plano, CRTC, conector, cálculo de PLL, detección de monitor |
| `fl2000_stream.c` | Envío continuo de frames por bulk USB |
| `fl2000_regs.c` | Registros del chip, secuencia de modo, maestro I2C |
| `fl2000_convert.h` | Conversión XRGB8888 → RGB888/RGB565 del chip (prueba en `test/`) |
| `fl2000_it66121.c` | Transmisor HDMI IT66121 opcional |

Prueba de la conversión de pixeles: `cc -O2 -o /tmp/t test/convert_test.c && /tmp/t`

## Créditos

- **Artem Mygaiev** (klogg/fl2000_drm): driver DRM original en el que se basa este.
- **Fresco Logic**: driver de referencia y definición de registros.
- **Samuel**: reescritura para kernels actuales (2026).

Licencia: GPL-2.0, ver [LICENSE](LICENSE).
