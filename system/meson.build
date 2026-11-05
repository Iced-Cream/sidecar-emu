system_ss.add(files(
  'vl.c',
), sdl, libpmem, libdaxctl)

system_ss.add(files(
  'arch_init.c',
  'balloon.c',
  'bootdevice.c',
  'cpus.c',
  'cpu-timers.c',
  'dirtylimit.c',
  'dma-helpers.c',
  'exit-with-parent.c',
  'globals.c',
  'ioport.c',
  'ram-block-attributes.c',
  'ram-discard-manager.c',
  'memory_mapping.c',
  'memory.c',
  'physmem.c',
  'physmem-qmp-cmds.c',
  'qdev-monitor.c',
  'qtest.c',
  'rtc.c',
  'runstate-action.c',
  'runstate.c',
  'watchpoint.c',
))

if have_hmp
endif

if have_tpm
endif

system_ss.add(when: 'CONFIG_DEVICE_TREE',
              if_true: [fdt, files('device_tree.c')])
stub_ss.add(files('device_tree-stub.c'))
if host_os == 'linux'
  system_ss.add(files('async-teardown.c'))
endif
