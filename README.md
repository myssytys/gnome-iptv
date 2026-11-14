# GNOME IPTV Player & Stream Recorder (`gnome-iptv`)

A native C++20, GTK4, and Libadwaita IPTV player, XMLTV EPG viewer, and FFmpeg stream recorder for Fedora GNOME.

## Features

- **M3U / M3U8 Playlist & Direct Stream Support**:
  - Connect to remote HTTPS/HTTP `.m3u` / `.m3u8` playlists (including Xtream Codes `get.php` URLs) or load local `.m3u` files via the file dialog or drag-and-drop.
  - Automatic detection of direct single-channel HLS `.m3u8` streams and automatic discovery of Xtream Codes `xmltv.php` / `url-tvg` EPG feeds.
- **Full XMLTV EPG & Configurable Auto-Update Timer**:
  - Dedicated EPG URL input and live auto-update countdown timer directly in the main sidebar.
  - Background XMLTV (`.xml` and `.xml.gz`) parser backed by SQLite (`~/.local/share/gnome-iptv/epg.db`) with instant Now/Next cards, interactive programme browsing (**Previous / Live / Next**), and global programme search.
- **Hardware-Accelerated Video Playback & Fullscreen Mode**:
  - Zero-transcode (`ffmpeg -c copy -f mpegts pipe:1`) stream pipeline fed into GTK4's hardware-accelerated `GtkMediaFile` (`GtkGstPaintable`) wrapped in `GtkGraphicsOffload` for smooth Wayland DMABUF scanout.
  - Toggle fullscreen via the toolbar button, `F11`, `F`, `Escape`, or double-clicking the video canvas.
  - Forward / Backward channel browsing via toolbar buttons or `Page Up` / `Page Down`.
- **Resilient Auto-Reconnect & FFmpeg DVR Stream Recording**:
  - Both live playback and active FFmpeg recordings automatically reconnect to the same stream URL if the stream breaks or stalls.
  - Record live streams immediately or schedule recordings directly from EPG programme rows (`.ts` or `.mkv`).

## Dependencies (Fedora)

```bash
sudo dnf install -y gcc-c++ meson ninja-build pkgconf-pkg-config \
    gtk4-devel libadwaita-devel libsoup3-devel libxml2-devel sqlite-devel ffmpeg
```

## Build & Run

```bash
meson setup build
meson compile -C build

# Run self-test suite
./build/gnome-iptv --self-test

# Launch application
./build/gnome-iptv
```
