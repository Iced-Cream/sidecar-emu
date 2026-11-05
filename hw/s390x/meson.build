s390x_ss = ss.source_set()
stub_ss.add(files('s390-pci-vfio-stubs.c'))
s390x_ss.add(when: 'CONFIG_VFIO_AP', if_false: files())

virtio_ss = ss.source_set()
if have_virtfs
endif
s390x_ss.add_all(when: 'CONFIG_VIRTIO_CCW', if_true: virtio_ss)

s390x_ss.add(when: 'CONFIG_VIRTIO_MD', if_false: files())

hw_arch += {'s390x': s390x_ss}

hw_s390x_modules = {}
virtio_gpu_ccw_ss = ss.source_set()
virtio_gpu_ccw_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_CCW'],
                      if_true: [files(), pixman])
hw_s390x_modules += {'virtio-gpu-ccw': virtio_gpu_ccw_ss}
modules += {'hw-s390x': hw_s390x_modules}
