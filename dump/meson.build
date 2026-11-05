system_ss.add([files('dump.c'), snappy, lzo])
if have_hmp
endif
system_ss.add(when: 'CONFIG_WINDUMP', if_false: files('win_dump-stubs.c'))
