project('friar', 'cpp', default_options: ['cpp_std=c++23'])

inc = include_directories('src')
src = files()

subdir('src')

executable(
  'friar',
  src,
  include_directories: inc,
)
