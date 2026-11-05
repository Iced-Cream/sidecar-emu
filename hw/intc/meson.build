system_ss.add(files('intc.c'))
system_ss.add(when: 'CONFIG_ARM_GIC', if_true: files(
  'arm_gic.c',
  'arm_gic_common.c',
  'arm_gicv2m.c',
  'arm_gicv3_common.c',
  'arm_gicv3_its_common.c',
))
system_ss.add(when: 'CONFIG_ARM_GICV3', if_true: files(
  'arm_gicv3.c',
  'arm_gicv3_dist.c',
  'arm_gicv3_its.c',
  'arm_gicv3_redist.c',
))
system_ss.add(when: 'CONFIG_ARM_GICV5', if_true: files(
  'arm_gicv5_common.c',
  'arm_gicv5.c',
))
system_ss.add(when: 'CONFIG_IOAPIC', if_true: files(), if_false: files('ioapic-stub.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_ic.c', 'bcm2836_control.c'))

if config_all_devices.has_key('CONFIG_APIC') or \
   config_all_devices.has_key('CONFIG_I8259') or \
   config_all_devices.has_key('CONFIG_MC146818RTC')
endif

arm_common_ss.add(when: 'CONFIG_ARM_GIC', if_true: files('arm_gicv3_cpuif_common.c'))
arm_common_ss.add(when: 'CONFIG_ARM_GICV3', if_true: files('arm_gicv3_cpuif.c'))
stub_ss.add(files('arm_gicv3_hvf_stub.c'))
arm_common_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m_nvic.c'))
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_OPENPIC'],
		if_true: files())
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_XICS'],
		if_true: files())
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_XIVE'],
		if_true: files())

specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_IPI'],
                if_true: files())
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_PCH_PIC'],
                if_true: files())
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_EXTIOI'],
               if_true: files())
specific_ss.add(when: ['CONFIG_KVM', 'CONFIG_LOONGARCH_DINTC'],
               if_true: files())
