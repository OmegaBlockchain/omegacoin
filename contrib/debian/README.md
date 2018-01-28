
Debian
====================
This directory contains files used to package omegacoind/omegacoin-qt
for Debian-based Linux systems. If you compile omegacoind/omegacoin-qt yourself, there are some useful files here.

## omegacoin: URI support ##


omegacoin-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install omegacoin-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your omegacoin-qt binary to `/usr/bin`
and the `../../share/pixmaps/omegacoin128.png` to `/usr/share/pixmaps`

omegacoin-qt.protocol (KDE)

