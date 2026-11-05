
# PCI network cards





if have_vhost_net
endif
stub_ss.add(files('vhost_net-stub.c'))


stub_ss.add(files('rocker/rocker-stubs.c'))
if have_hmp
endif

system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2838_genet.c'))

subdir('can')
