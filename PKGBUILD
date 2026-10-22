# Build from a checkout of this repository: makepkg -si
pkgname=fl2000-dkms
pkgver=1.0.0
pkgrel=1
pkgdesc="DRM driver for Fresco Logic FL2000 USB 3.0 to VGA/HDMI adapters (DKMS)"
arch=('any')
url="https://github.com/Samuv5/fl2000_drm"
license=('GPL-2.0-only')
depends=('dkms')

package() {
	local dest="$pkgdir/usr/src/fl2000-$pkgver"

	install -Dm644 -t "$dest" "$startdir"/{Kbuild,Makefile,dkms.conf,*.c,*.h}
	sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$pkgver\"/" "$dest/dkms.conf"
}
