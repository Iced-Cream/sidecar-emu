i386_ss = ss.source_set()

i386_ss.add(when: 'CONFIG_X86_IOMMU', if_true: files(),
                                      if_false: files())
i386_ss.add(when: 'CONFIG_AMD_IOMMU', if_true: files(),
                                      if_false: files())
i386_ss.add(when: 'CONFIG_SGX', if_true: files(),
                                if_false: files())
if have_hmp
endif

i386_ss.add(when: 'CONFIG_X86_FW_OVMF', if_true: files(),
                                        if_false: files())

subdir('kvm')
subdir('mshv')
subdir('xen')

i386_ss.add_all(xenpv_ss)

hw_arch += {'i386': i386_ss}
