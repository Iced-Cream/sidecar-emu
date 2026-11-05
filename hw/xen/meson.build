
xen_common_ss = ss.source_set()

if have_xen_pci_passthrough
else
endif

system_ss.add_all(when: ['CONFIG_XEN', xen], if_true: xen_common_ss)
stub_ss.add(files('xen_stubs.c'))
