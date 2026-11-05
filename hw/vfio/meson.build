# SPDX-License-Identifier: GPL-2.0-or-later

vfio_ss = ss.source_set()
stub_ss.add(files('kvm-stubs.c'))
stub_ss.add(files('igd-stubs.c'))

system_ss.add_all(when: 'CONFIG_VFIO', if_true: vfio_ss)

stub_ss.add(files('iommufd-stubs.c'))
