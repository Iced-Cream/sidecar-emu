arm_common_ss = ss.source_set()

arm_common_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))
arm_common_ss.add(when: 'CONFIG_RASPI', if_true: files(
  'bcm2836.c',
  'bcm2838.c',
  'raspi.c',
  'raspi4b.c'
))
stub_ss.add(files('smmuv3-accel-stubs.c'))
stub_ss.add(files('tegra241-cmdqv-stubs.c'))

arm_common_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_peripherals.c'))
arm_common_ss.add(when: 'CONFIG_RASPI', if_true: files(
    'bcm2838_peripherals.c',
    'bcm2838_pcie.c'))


arm_common_ss.add(files('boot.c'))

hw_common_arch += {'arm': arm_common_ss}
