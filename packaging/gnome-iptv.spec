Name:           gnome-iptv
Version:        1.0.0
Release:        1%{?dist}
Summary:        Native GTK4/Libadwaita IPTV Player & Stream Recorder

License:        GPL-3.0-or-later
URL:            https://github.com/myssytys/gnome-iptv
Source0:        gnome-iptv-1.0.0.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0.0
BuildRequires:  ninja-build
BuildRequires:  desktop-file-utils

Requires:       gtk4 >= 4.14
Requires:       libadwaita-1 >= 1.5
Requires:       libsoup-3.0 >= 3.0
Requires:       libxml2 >= 2.9
Requires:       sqlite-libs >= 3.30
Requires:       ffmpeg
Requires:       mpv
Requires:       hicolor-icon-theme

# Disable debuginfo package generation for standalone binary release
%define debug_package %{nil}
%define __arch_install_post %{nil}
%define __os_install_post %{nil}

%description
GNOME IPTV Player & Stream Recorder is a modern, responsive Linux IPTV application
built natively with C++20, GTK4, and Libadwaita. It supports HTTPS M3U playlists,
Xtream Codes auto-discovery, XMLTV EPG guides, continuous live recording with
automatic reconnection, and GNOME Shell system tray minimization.

%prep
%autosetup -n gnome-iptv-1.0.0

%build
meson setup rpmbuild_dir --prefix=%{_prefix} --sysconfdir=%{_sysconfdir} --localstatedir=%{_localstatedir} --buildtype=release
meson compile -C rpmbuild_dir

%install
DESTDIR=%{buildroot} meson install -C rpmbuild_dir
desktop-file-validate %{buildroot}%{_datadir}/applications/org.gnome.IptvPlayer.desktop

%files
%{_bindir}/gnome-iptv
%{_datadir}/applications/org.gnome.IptvPlayer.desktop
%{_datadir}/icons/hicolor/*/apps/gnome-iptv.png
%{_datadir}/icons/hicolor/*/apps/org.gnome.IptvPlayer.png
%{_datadir}/pixmaps/gnome-iptv.png
%{_datadir}/pixmaps/org.gnome.IptvPlayer.png

%changelog
* Mon Nov 16 2026 myssy <myssy@users.noreply.github.com> - 1.0.0-1
- Initial Fedora RPM package with native GNOME icon and tray integration.
