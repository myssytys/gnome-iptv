project('gnome-iptv', 'cpp',
  version: '1.0.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ]
)

gnome = import('gnome')

gtk4_dep       = dependency('gtk4', version: '>= 4.14')
adwaita_dep    = dependency('libadwaita-1', version: '>= 1.5')
soup_dep       = dependency('libsoup-3.0', version: '>= 3.0')
xml2_dep       = dependency('libxml-2.0', version: '>= 2.9')
sqlite3_dep    = dependency('sqlite3', version: '>= 3.30')
threads_dep    = dependency('threads')

sources = files(
  'src/main.cpp',
  'src/application.cpp',
  'src/window.cpp',
  'src/preferences_dialog.cpp',
  'src/models/types.cpp',
  'src/services/config_store.cpp',
  'src/services/m3u_parser.cpp',
  'src/services/epg_manager.cpp',
  'src/services/stream_player.cpp',
  'src/services/stream_recorder.cpp',
)

executable('gnome-iptv',
  sources,
  include_directories: include_directories('src'),
  dependencies: [
    gtk4_dep,
    adwaita_dep,
    soup_dep,
    xml2_dep,
    sqlite3_dep,
    threads_dep,
  ],
  install: true,
)

install_data('data/org.gnome.IptvPlayer.desktop',
  install_dir: get_option('datadir') / 'applications'
)

