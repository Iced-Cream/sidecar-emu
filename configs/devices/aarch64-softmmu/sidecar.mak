# Sidecar emulator: the Raspberry Pi 4B machine and the USB devices used in the gates
CONFIG_RASPI=y
CONFIG_USB_RYTM=y
CONFIG_USB_HUB=y
CONFIG_USB_HID=y
# target/arm links the GICv5 CPU interface for every aarch64 TCG build
CONFIG_ARM_GICV5=y
