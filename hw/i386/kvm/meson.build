i386_kvm_ss = ss.source_set()

i386_ss.add_all(when: 'CONFIG_KVM', if_true: i386_kvm_ss)

stub_ss.add(files(
  'xen-stubs.c',
))
