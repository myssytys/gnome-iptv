Name:           gnome-iptv
Version:        1.1.0
Release:        1%{?dist}
Summary:        Native GTK4/Libadwaita IPTV Player & Stream Recorder

License:        GPL-3.0-or-later
URL:            https://github.com/myssytys/gnome-iptv
Source0:        gnome-iptv-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0.0
BuildRequires:  ninja-build
BuildRequires:  desktop-file-utils

Requires:       gtk4 >= 4.14
Requires:       libadwaita >= 1.5
Requires:       libsoup3 >= 3.0
Requires:       libxml2 >= 2.9
Requires:       sqlite-libs >= 3.30
Requires:       hicolor-icon-theme
Requires:       /usr/bin/ffmpeg

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
%autosetup -n gnome-iptv-%{version}

%build
meson setup rpmbuild_dir --prefix=%{_prefix} --sysconfdir=%{_sysconfdir} --localstatedir=%{_localstatedir} --buildtype=release
meson compile -C rpmbuild_dir

%install
DESTDIR=%{buildroot} meson install -C rpmbuild_dir
desktop-file-validate %{buildroot}%{_datadir}/applications/org.gnome.IptvPlayer.desktop

%post
/bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
update-desktop-database &>/dev/null || :

%postun
if [ $1 -eq 0 ] ; then
    /bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
    gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
fi
update-desktop-database &>/dev/null || :

%posttrans
gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :

%files
%{_bindir}/gnome-iptv
%{_datadir}/applications/org.gnome.IptvPlayer.desktop
%{_datadir}/icons/hicolor/*/apps/gnome-iptv.png
%{_datadir}/icons/hicolor/*/apps/org.gnome.IptvPlayer.png
%{_datadir}/pixmaps/gnome-iptv.png
%{_datadir}/pixmaps/org.gnome.IptvPlayer.png

%changelog
* Mon Nov 16 2026 myssy <myssy@users.noreply.github.com> - 1.1.0-1
- Record live streams to .ts and automatically remux to .mkv via FFmpeg upon completion.

* Mon Nov 16 2026 myssy <myssy@users.noreply.github.com> - 1.0.0-1
- Initial Fedora RPM package with native GNOME icon and tray integration.
