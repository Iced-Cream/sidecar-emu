keymaps = [
  ['atset1', 'linux'],
  ['linux', 'qcode'],
  ['linux', 'atset1'],
  ['linux', 'atset2'],
  ['linux', 'atset3'],
  ['qcode', 'linux'],
  ['linux', 'qnum'],
  ['linux', 'sun'],
  ['qnum', 'linux'],
  ['usb', 'linux'],
  ['win32', 'linux'],
  ['x11', 'linux'],
  ['xorgkbd', 'linux'],
  ['xorgxquartz', 'linux'],
  ['xorgxwin', 'linux'],
  ['osx', 'linux'],
]

if have_system or xkbcommon.found()
  keycodemapdb_proj = subproject('keycodemapdb', required: true)
  foreach e : keymaps
    output = 'input-keymap-@0@-to-@1@.c.inc'.format(e[0], e[1])
    genh += custom_target(output,
                  output: output,
                  capture: true,
                  input: keycodemapdb_proj.get_variable('keymaps_csv'),
                  command: [python, keycodemapdb_proj.get_variable('keymap_gen').full_path(),
                            'code-map', '--lang', 'glib2',
                            '--varname', 'qemu_input_map_@0@_to_@1@'.format(e[0], e[1]),
                            '@INPUT0@', e[0], e[1]])
  endforeach
endif

libui_sources = files(
    'clipboard.c',
    'console.c',
    'cursor.c',
    'dmabuf.c',
    'display-surface.c',
    'input-keymap.c',
    'kbd-state.c',
    'keymaps.c',
    'qemu-pixman.c',
    'spice-module.c',
    'vgafont.c',
  )
if pixman.found()
  libui_sources += files( )
endif
libui = static_library('qemuui', libui_sources + genh,
  dependencies: [pixman, spice_headers],
  build_by_default: false)
ui = declare_dependency(objects: libui.extract_all_objects(recursive: false), dependencies: [pixman])
system_ss.add(png)
system_ss.add(files(
  'input-barrier.c',
  'input.c',
  'ui-qmp-cmds.c',
  'util.c',
))
if have_hmp
endif
system_ss.add(ui)
system_ss.add(when: pixman, if_true: files(), if_false: files('console-vc-stubs.c'))
if dbus_display
endif

if host_os == 'linux'
  system_ss.add(files('input-linux.c', 'udmabuf.c'))
endif

vnc_ss = ss.source_set()
vnc_ss.add(zlib, jpeg, png)
system_ss.add_all(when: [vnc, pixman], if_true: vnc_ss)
system_ss.add(when: vnc, if_true: files(), if_false: files('vnc-stubs.c'))

ui_modules = {}

if curses.found()
  curses_ss = ss.source_set()
  curses_ss.add(when: [curses, iconv], if_true: [files(), pixman])
  ui_modules += {'curses' : curses_ss}
endif

system_ss.add(opengl)
if opengl.found()
  opengl_ss = ss.source_set()
  opengl_ss.add(gbm, pixman)
  opengl_ss.add(when: [opengl],
               if_true: files(   ))
  ui_modules += {'opengl' : opengl_ss}
endif

if opengl.found()
  egl_headless_ss = ss.source_set()
  egl_headless_ss.add(when: [opengl, pixman],
                      if_true: [files(), gbm])
  ui_modules += {'egl-headless' : egl_headless_ss}
endif

dbus_display1 = files()
if dbus_display
  dbus_ss = ss.source_set()
  env = environment()
  env.set('HOST_OS', host_os)
  xml = custom_target('dbus-display preprocess',
                      input: 'dbus-display1.xml',
                      output: 'dbus-display1.xml',
                      env: env,
                      command: [xml_pp, '@INPUT@', '@OUTPUT@'])
  dbus_display1 = custom_target('dbus-display gdbus-codegen',
                                output: ['dbus-display1.h', 'dbus-display1.c'],
                                input: xml,
                                command: [gdbus_codegen, '@INPUT@',
                                          '--glib-min-required', '2.64',
                                          '--output-directory', meson.current_build_dir(),
                                          '--interface-prefix', 'org.qemu.',
                                          '--c-namespace', 'QemuDBus',
                                          '--generate-c-code', '@BASENAME@'])
  dbus_display1_h = declare_dependency(dependencies: [gio],
                                       sources: dbus_display1[0])
  dbus_ss.add(when: gio,
              if_true: [files(
                
                
                
                
                
                
              ), opengl, gbm, pixman, dbus_display1])
  ui_modules += {'dbus' : dbus_ss}
endif

if gtk.found()
  if host_os == 'windows'
  endif

  gtk_ss = ss.source_set()
  gtk_ss.add(gtk, vte, pixman, files())
  ui_modules += {'gtk' : gtk_ss}
endif

if sdl.found()
  sdl_ss = ss.source_set()
  sdl_ss.add(sdl, sdl_image, pixman, glib, files(
    
    
    
  ))
  ui_modules += {'sdl' : sdl_ss}
endif

if spice.found()
  spice_core_ss = ss.source_set()
  spice_core_ss.add(spice, pixman, files(
    
    
    
  ))
  ui_modules += {'spice-core' : spice_core_ss}

  if gio.found()
    spice_ss = ss.source_set()
    spice_ss.add(spice, gio, pixman, files())
    ui_modules += {'spice-app': spice_ss}
  endif
endif

subdir('shader')

if have_system
  subdir('icons')

  install_data('qemu.desktop', install_dir: qemu_desktopdir)
endif

modules += {'ui': ui_modules}
