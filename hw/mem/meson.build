mem_ss = ss.source_set()
stub_ss.add(files('cxl_type3_stubs.c'))

stub_ss.add(files('memory-device-stubs.c'))
system_ss.add_all(when: 'CONFIG_MEM_DEVICE', if_true: mem_ss)

