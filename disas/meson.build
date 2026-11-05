common_ss.add(when: capstone, if_true: [files(), capstone])
common_ss.add(when: 'CONFIG_TCG', if_true: files(
    'disas-host.c',
    'disas-target.c',
    'objdump.c'
))
common_ss.add(files('disas-common.c'))
if have_hmp
endif
specific_ss.add(capstone)
