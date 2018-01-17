
Debian
====================
This directory contains files used to package super7coind/super7coin-qt
for Debian-based Linux systems. If you compile super7coind/super7coin-qt yourself, there are some useful files here.

## super7coin: URI support ##


super7coin-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install super7coin-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your super7coin-qt binary to `/usr/bin`
and the `../../share/pixmaps/super7coin128.png` to `/usr/share/pixmaps`

super7coin-qt.protocol (KDE)

