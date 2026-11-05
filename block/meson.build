block_ss.add(genh)
block_ss.add(files(
  'accounting.c',
  'aio_task.c',
  'amend.c',
  'backup.c',
  'blkdebug.c',
  'blklogwrites.c',
  'blkverify.c',
  'block-backend.c',
  'block-copy.c',
  'commit.c',
  'copy-before-write.c',
  'copy-on-read.c',
  'create.c',
  'crypto.c',
  'dirty-bitmap.c',
  'filter-compress.c',
  'graph-lock.c',
  'io.c',
  'mirror.c',
  'nbd.c',
  'null.c',
  'preallocate.c',
  'progress_meter.c',
  'qapi.c',
  'qcow2.c',
  'qcow2-bitmap.c',
  'qcow2-cache.c',
  'qcow2-cluster.c',
  'qcow2-refcount.c',
  'qcow2-snapshot.c',
  'qcow2-threads.c',
  'quorum.c',
  'raw-format.c',
  'reqlist.c',
  'snapshot.c',
  'snapshot-access.c',
  'throttle.c',
  'throttle-groups.c',
  'write-threshold.c',
), zstd, zlib)

system_ss.add(when: 'CONFIG_TCG', if_true: files('blkreplay.c'))
system_ss.add(files('block-ram-registrar.c'))

if get_option('qcow1').allowed()
endif
if get_option('vdi').allowed()
endif
if get_option('vhdx').allowed()
endif
if get_option('vmdk').allowed()
endif
if get_option('vpc').allowed()
endif
if get_option('cloop').allowed()
endif
if get_option('bochs').allowed()
endif
if get_option('vvfat').allowed()
endif
if get_option('dmg').allowed()
endif
if get_option('qed').allowed()
endif
if get_option('parallels').allowed()
endif

if host_os == 'windows'
else
  block_ss.add(files('file-posix.c'), coref, iokit)
endif
if host_os == 'linux'
  block_ss.add(files('nvme.c'))
endif
if get_option('replication').allowed()
endif

block_modules = {}

modsrc = []
foreach m : [
  [blkio, 'blkio', files()],
  [curl, 'curl', files()],
  [libiscsi, 'iscsi', files()],
  [libnfs, 'nfs', files()],
  [libssh, 'ssh', files()],
  [rbd, 'rbd', files()],
]
  if m[0].found()
    module_ss = ss.source_set()
    module_ss.add(when: m[0], if_true: m[2])
    if enable_modules
      modsrc += m[2]
    endif
    block_modules += {m[1] : module_ss}
  endif
endforeach

# those are not exactly regular block modules, so treat them apart
if get_option('dmg').allowed()
  foreach m : [
    [liblzfse, 'dmg-lzfse', liblzfse, 'dmg-lzfse.c'],
    [libbzip2, 'dmg-bz2', [glib, libbzip2], 'dmg-bz2.c']
  ]
    if m[0].found()
      module_ss = ss.source_set()
      module_ss.add(when: m[2], if_true: files(m[3]))
      block_modules += {m[1] : module_ss}
    endif
  endforeach
endif

module_block_py = find_program('../scripts/modules/module_block.py')
module_block_h = custom_target('module_block.h',
                               output: 'module_block.h',
                               input: modsrc,
                               command: [module_block_py, '@OUTPUT0@', modsrc])
block_ss.add(module_block_h)

wrapper_py = find_program('../scripts/block-coroutine-wrapper.py')
block_gen_c = custom_target('block-gen.c',
                            output: 'block-gen.c',
                            input: files(
                                      '../include/block/block-io.h',
                                      '../include/block/dirty-bitmap.h',
                                      '../include/block/block_int-io.h',
                                      '../include/block/block-global-state.h',
                                      '../include/system/block-backend-global-state.h',
                                      '../include/system/block-backend-io.h',
                                      'coroutines.h'
                                      ),
                            command: [wrapper_py, '@OUTPUT@', '@INPUT@'])
block_ss.add(block_gen_c)

block_ss.add(files('stream.c'))

system_ss.add(files('qapi-system.c'))

subdir('export')
subdir('monitor')

modules += {'block': block_modules}
