system_virtio_ss = ss.source_set()


if have_vhost
  if have_vhost_user

    # MMIO Stubs

    # PCI Stubs
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_TEST'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_GPIO'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_I2C'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_RNG'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_SND'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_INPUT'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_SPI'],
                         if_true: files())
    system_virtio_ss.add(when: ['CONFIG_VIRTIO_PCI', 'CONFIG_VHOST_USER_RTC'],
                         if_true: files())
  endif
  if have_vhost_vdpa
  endif
endif

system_virtio_ss.add(when: 'CONFIG_VIRTIO_NSM', if_true: [files(), libcbor])

virtio_pci_ss = ss.source_set()

virtio_pci_ss.add(when: 'CONFIG_VIRTIO_NSM', if_true: [files( ), libcbor])

system_virtio_ss.add_all(when: 'CONFIG_VIRTIO_PCI', if_true: virtio_pci_ss)

system_ss.add_all(when: 'CONFIG_VIRTIO', if_true: system_virtio_ss)
stub_ss.add(files('vhost-stub.c'))
stub_ss.add(files('vhost-user-stub.c'))
stub_ss.add(files('virtio-stub.c'))
stub_ss.add(files('virtio-md-stubs.c'))

if have_hmp
endif

