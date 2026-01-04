project('pipewire-3d-mixer', 'c',
  version: '1.0.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
  ],
)

# Dependencies
gtk_dep = dependency('gtk4', version: '>= 4.0')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.0')
spa_dep = dependency('libspa-0.2', version: '>= 0.2')
glib_dep = dependency('glib-2.0', version: '>= 2.66')
math_dep = meson.get_compiler('c').find_library('m', required: true)

# Sources
sources = files(
  'app.c',
  'main.c',
  'pipewire.c',
  'ui.c',
)

# Executable
executable('pw-3d-mixer',
  sources,
  dependencies: [
    gtk_dep,
    pipewire_dep,
    spa_dep,
    glib_dep,
    math_dep,
  ],
  install: true,
)

# Desktop file (optional)
install_data('pw-3d-mixer.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'),
)
