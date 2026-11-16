# Import-only dependencies

These components prepare installation media. They are not called by the VM's
normal disk read/write path. The installer uses an ordinary raw image afterwards.

- **libudfread 1.2.0**: UDF ISO reader, LGPL-2.1-or-later. Source files are unmodified.
  Source: https://download.videolan.org/pub/videolan/libudfread/libudfread-1.2.0.tar.xz
  License: `libudfread/COPYING`.
- **FatFs R0.16**: FAT32 installation-media writer, ChaN's permissive license.
  Source: https://elm-chan.org/fsw/ff/arc/ff16.zip
  Source files are unmodified apart from the application configuration in `ffconf.h`.
  Copyright and license notices are retained in each source file.
- **libwim 1.14.5**: splits large `install.wim` files into standard `install*.swm`
  files during import. Built without NTFS-3G/FUSE under LGPL-2.1-or-later.
  Source: https://wimlib.net/downloads/wimlib-1.14.5.tar.gz
  `scripts/build_ios_import_deps.sh` downloads the source into `work/iso-deps`,
  builds the static archive in `ios/deps/lib/libwim.a`, and copies the public
  header to `ios/deps/include/wimlib.h`. Its full source and licensing notices
  remain in that source archive and extracted build directory.

The app includes the UDF/libwim licenses and FatFs notice in its `Licenses`
resource directory. Rebuilding the app with the supplied sources and generated
static archives permits relinking these dependencies.
