project('elevende', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=1'])

cc = meson.get_compiler('c')

wlroots = dependency('wlroots', version: ['>=0.17.0', '<0.18.0'])
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_scanner = find_program('wayland-scanner')

# Ubuntu's libwlroots-dev does not ship the generated xdg-shell protocol
# header that <wlr/types/wlr_xdg_shell.h> includes, so generate it here.
xdg_shell_xml = '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
xdg_shell_h = custom_target('xdg-shell-server-header',
  input: xdg_shell_xml,
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_shell_c = custom_target('xdg-shell-private-code',
  input: xdg_shell_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
layer_shell_h = custom_target('layer-shell-server-header',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
layer_shell_c = custom_target('layer-shell-private-code',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

executable('elevende',
  ['elevende.c', layer_shell_c, layer_shell_h, xdg_shell_h, xdg_shell_c],
  dependencies: [wlroots, wayland_server, xkbcommon],
  install: true)
