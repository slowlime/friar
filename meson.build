project(
  'friar',
  'c', 'cpp',
  meson_version: '>=1.1',
  default_options: ['cpp_std=c++23'],
)

src = files()

subdir('src')

runtime_path = get_option('runtime_path')

cc = meson.get_compiler('c')
runtime_dep = cc.find_library(
  'runtime',
  dirs: meson.current_source_dir() / runtime_path,
  static: true,
  has_headers: ['gc.h'],
  header_include_directories: include_directories(runtime_path),
)

executable(
  'friar',
  src,
  dependencies: [runtime_dep],
  include_directories: include_directories(
    runtime_path,
  ),
)
