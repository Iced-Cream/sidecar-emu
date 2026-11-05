pci_ss = ss.source_set()
pci_ss.add(when: 'CONFIG_PCIE_PORT', if_true: files('pcie_root_port.c', 'gen_pcie_root_port.c',
                                                    'gen_pcie_upstream_port.c', 'gen_pcie_downstream_port.c'))

# Sun4u

system_ss.add_all(when: 'CONFIG_PCI', if_true: pci_ss)
