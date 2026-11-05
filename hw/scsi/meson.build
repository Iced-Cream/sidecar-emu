scsi_ss = ss.source_set()
specific_scsi_ss = ss.source_set()
virtio_scsi_ss = ss.source_set()




scsi_ss.add_all(when: 'CONFIG_VIRTIO_SCSI', if_true: virtio_scsi_ss)


system_ss.add_all(when: 'CONFIG_SCSI', if_true: scsi_ss)
specific_ss.add_all(when: 'CONFIG_SCSI', if_true: specific_scsi_ss)
