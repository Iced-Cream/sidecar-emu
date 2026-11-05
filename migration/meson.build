# Files needed by unit tests
migration_files = files(
  'migration-stats.c',
  'page_cache.c',
  'xbzrle.c',
  'vmstate-types.c',
  'vmstate.c',
  'qemu-file.c',
  'yank_functions.c',
)

system_ss.add(files(
  'block-dirty-bitmap.c',
  'block-active.c',
  'channel.c',
  'channel-block.c',
  'cpr.c',
  'cpr-transfer.c',
  'cpr-exec.c',
  'cpu-throttle.c',
  'dirtyrate.c',
  'exec.c',
  'fd.c',
  'file.c',
  'global_state.c',
  'migration.c',
  'multifd.c',
  'multifd-device-state.c',
  'multifd-nocomp.c',
  'multifd-zlib.c',
  'multifd-zero-page.c',
  'options.c',
  'postcopy-ram.c',
  'ram.c',
  'savevm.c',
  'socket.c',
  'tls.c',
), gnutls, zlib)
if have_hmp
  system_ss.add([spice_headers, files(), spice])
endif

if get_option('replication').allowed()
else
  system_ss.add(files('colo-stubs.c'))
endif

system_ss.add(when: 'CONFIG_VFIO',
              if_true: files())
stub_ss.add(files('vfio-stub.c'))
