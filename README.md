# fl2000 — DRM driver for Fresco Logic FL2000 USB 3.0 display adapters

[Español](README.es.md)

Linux kernel driver for USB 3.0 to VGA / HDMI adapters based on the **Fresco Logic FL2000DX**
chip (USB ID `1d5c:2000`). The adapter shows up as one more graphics card (`/dev/dri/cardN`), so
it works with any desktop: GNOME and KDE on Wayland, Cinnamon and others on X11, and the text
console.

Tested with a Steren VGA+HDMI adapter on CachyOS (kernel 7.2, clang) running GNOME Wayland at
1920x1080 @ 60 Hz. Also builds on 6.18 LTS kernels and on 7.0 with gcc.

## Installation (CachyOS / Arch)

```sh
sudo pacman -S --needed base-devel dkms linux-cachyos-headers   # or your kernel's headers
git clone https://github.com/Samuv5/fl2000_drm
cd fl2000_drm
makepkg -si
```

The `fl2000-dkms` package rebuilds the module automatically on every kernel update (it detects
clang-built kernels on its own). Plug in the adapter and you are done; if your desktop session
was already running, log out and back in so the desktop picks it up.

### Other distributions / try without installing

```sh
make
sudo insmod fl2000.ko
```

## Usage and settings

- Resolution and layout: in your desktop's display settings.
- `max_bpp`: bits per pixel sent over USB, `24` (default) or `16`. 16 uses half the bandwidth,
  useful on slow USB ports or hubs:
  `echo 16 | sudo tee /sys/module/fl2000/parameters/max_bpp` (applies on the next mode change)
  or `options fl2000 max_bpp=16` in `/etc/modprobe.d/fl2000.conf`.
- Debug messages: `sudo insmod fl2000.ko dyndbg=+p`.

## Known limitations

- Needs USB 3.0. Over USB 2.0 only very small resolutions fit.
- If the adapter gives no access to the monitor's EDID (like the tested Steren), the VGA monitor
  is detected by load sensing and standard modes up to 1920x1200 are offered.
- HDMI adapters with an ITE IT66121 chip: support ported from the original driver, untested on
  hardware. Adapters that convert VGA to HDMI with another chip work as VGA.
- No HDMI audio.
- GNOME does not always pick up the adapter when it is plugged in during a session (a mutter
  limitation): log out and back in.

## Design

A single module (`fl2000.ko`) following the pattern of the kernel's USB display drivers (`udl`,
`gud`): GEM SHMEM with a shadow plane, PRIME to receive the image from the main GPU, no real
vblank. The chip programming sequence and the streaming protocol follow
[Fresco Logic's reference driver](https://github.com/FrescoLogic/FL2000).

| File | Contents |
|---|---|
| `fl2000_drv.c` | USB and DRM device, interrupt endpoint, suspend |
| `fl2000_modeset.c` | Plane, CRTC, connector, PLL calculation, monitor detection |
| `fl2000_stream.c` | Continuous frame streaming over USB bulk |
| `fl2000_regs.c` | Chip registers, mode sequence, I2C master |
| `fl2000_convert.h` | XRGB8888 → chip RGB888/RGB565 conversion (test in `test/`) |
| `fl2000_it66121.c` | Optional IT66121 HDMI transmitter |

Pixel conversion test: `cc -O2 -o /tmp/t test/convert_test.c && /tmp/t`

## Credits

- **Artem Mygaiev** (klogg/fl2000_drm): original DRM driver this one is based on.
- **Fresco Logic**: reference driver and register definitions.
- **Samuel**: rewrite for current kernels (2026).

License: GPL-2.0, see [LICENSE](LICENSE).

---

Thanks Claude for cleaning this trash, been 1 year without a 2nd monitor and no model fixed it like you did, yay!
