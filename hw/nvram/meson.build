system_ss.add(files('fw_cfg-interface.c'))
system_ss.add(files('fw_cfg.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_otp.c'))


