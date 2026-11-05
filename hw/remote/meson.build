remote_ss = ss.source_set()


remote_ss.add(when: 'CONFIG_VFIO_USER_SERVER', if_true: libvfio_user_dep)
remote_ss.add(when: 'CONFIG_VFIO_USER_SERVER', if_true: files(),
              if_false: files())


system_ss.add_all(when: 'CONFIG_MULTIPROCESS', if_true: remote_ss)
