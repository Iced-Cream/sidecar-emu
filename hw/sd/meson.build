system_ss.add(when: 'CONFIG_SD', if_true: files('sd.c', 'core.c'))
system_ss.add(when: 'CONFIG_SDHCI', if_true: files('sdhci.c'))

system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_sdhost.c'))
