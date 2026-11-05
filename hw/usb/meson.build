hw_usb_modules = {}

# usb subsystem core
system_ss.add(when: 'CONFIG_USB', if_true: files(
  'bus.c',
  'combined-packet.c',
  'core.c',
  'desc.c',
  'desc-msos.c',
  'libhw.c',
  'pcap.c',
))
stub_ss.add(files('bus-stub.c'))

# usb host adapters
system_ss.add(when: 'CONFIG_USB_XHCI', if_true: files('hcd-xhci.c'))
system_ss.add(when: 'CONFIG_USB_XHCI_PCI', if_true: files('hcd-xhci-pci.c'))
system_ss.add(when: 'CONFIG_USB_XHCI_VL805', if_true: files('hcd-xhci-vl805.c'))
system_ss.add(when: 'CONFIG_USB_MIDI_CODEC', if_true: files('usbmidi-codec.c'))
system_ss.add(when: 'CONFIG_USB_RYTM', if_true: files('dev-rytm.c'))
system_ss.add(when: 'CONFIG_USB_DWC2', if_true: files('hcd-dwc2.c', 'dwc2-gadget.c'))


# emulated usb devices
system_ss.add(when: 'CONFIG_USB_HUB', if_true: files('dev-hub.c'))
system_ss.add(when: 'CONFIG_USB_HID', if_true: files('dev-hid.c'))
if host_os != 'windows'
endif

# smartcard

if cacard.found()
  usbsmartcard_ss = ss.source_set()
  usbsmartcard_ss.add(when: 'CONFIG_USB_SMARTCARD',
                      if_true: [cacard, files( )])
  hw_usb_modules += {'smartcard': usbsmartcard_ss}
endif

# U2F
if host_os == 'linux'
  system_ss.add(when: 'CONFIG_USB_U2F', if_true: [libudev, files()])
endif
if u2f.found()
  system_ss.add(when: 'CONFIG_USB_U2F', if_true: [u2f, files()])
endif

# CanoKey
if canokey.found()
  system_ss.add(when: 'CONFIG_USB_CANOKEY', if_true: [canokey, files()])
endif

# usb redirect
if usbredir.found()
  usbredir_ss = ss.source_set()
  usbredir_ss.add(when: 'CONFIG_USB',
                  if_true: [usbredir, files( )])
  hw_usb_modules += {'redirect': usbredir_ss}
endif

# usb pass-through
if libusb.found()
  usbhost_ss = ss.source_set()
  usbhost_ss.add(when: ['CONFIG_USB', libusb],
                 if_true: files())
  hw_usb_modules += {'host': usbhost_ss}
endif


modules += { 'hw-usb': hw_usb_modules }
