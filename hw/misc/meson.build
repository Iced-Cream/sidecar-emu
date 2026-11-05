
system_ss.add(when: 'CONFIG_UNIMP', if_true: files('unimp.c'))

# ARM devices

system_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m_ras.c'))

# Mac devices
stub_ss.add(files('mos6522-stub.c'))

# virt devices

# RISC-V devices

subdir('macio')

# ivshmem devices

system_ss.add(when: 'CONFIG_RASPI', if_true: files(
  'bcm2835_mbox.c',
  'bcm2835_mphi.c',
  'bcm2835_property.c',
  'bcm2835_rng.c',
  'bcm2835_thermal.c',
  'bcm2835_cprman.c',
  'bcm2835_powermgt.c',
  'bcm2838_rng200.c',
  'bcm2838_thermal.c',
))









if igvm.found()
endif


# HPPA devices


# Qualcomm Hexagon DSP subsystems
