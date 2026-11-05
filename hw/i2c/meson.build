i2c_ss = ss.source_set()
i2c_ss.add(when: 'CONFIG_I2C', if_true: files('core.c'))
i2c_ss.add(when: 'CONFIG_BCM2835_I2C', if_true: files('bcm2835_i2c.c'))
system_ss.add_all(when: 'CONFIG_I2C', if_true: i2c_ss)
