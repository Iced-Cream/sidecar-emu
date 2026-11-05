system_ss.add([files(
  'cryptodev-builtin.c',
  'cryptodev.c',
  'hostmem-ram.c',
  'hostmem.c',
  'rng-builtin.c',
  'rng-egd.c',
  'rng.c',
  'confidential-guest-support.c',
), numa])

if have_hmp
endif

if host_os != 'windows'
  system_ss.add(files('rng-random.c'))
  if host_os != 'emscripten'
    system_ss.add(files('hostmem-file.c'))
    system_ss.add([files('hostmem-shm.c'), rt])
  endif
endif
if host_os == 'linux'
  system_ss.add(files('hostmem-memfd.c'))
  system_ss.add(files('host_iommu_device.c'))
endif
if keyutils.found()
    system_ss.add(keyutils, files())
endif
if have_vhost_user
endif
if have_vhost_user_crypto
endif

system_ss.add(when: igvm, if_true: [files( ), fdt])


subdir('tpm')
