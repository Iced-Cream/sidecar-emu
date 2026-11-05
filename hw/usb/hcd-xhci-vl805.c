/*
 * VIA VL805 USB 3.0 host controller, as on the Raspberry Pi 4B's PCIe bus
 *
 * A qemu-xhci with the VL805's PCI IDs and port layout (port 1 USB 2, ports
 * 2-5 USB 3), legacy INTx only (the BCM2711 PCIe model delivers no MSI),
 * and scratchpad buffers advertised, as bare-metal drivers for the real
 * chip (Circle) expect them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/usb/usb.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"

#include "hcd-xhci-pci.h"

#define TYPE_VL805_XHCI "vl805"
#define PCI_DEVICE_ID_VIA_VL805 0x3483

static void vl805_xhci_instance_init(Object *obj)
{
    XHCIPciState *pci = XHCI_PCI(obj);

    /* As Circle expects the VL805: port 1 USB 2 (the board's hub), 2-5 USB 3 */
    pci->xhci.numports_2 = 1;
    pci->xhci.numports_3 = 4;
    pci->xhci.usb2_first = true;
    pci->xhci.numintrs = XHCI_MAXINTRS;
    pci->xhci.numslots = XHCI_MAXSLOTS;
    pci->xhci.scratchpad_bufs = 1;
    pci->msi = ON_OFF_AUTO_OFF;
    pci->msix = ON_OFF_AUTO_OFF;
}

static void vl805_xhci_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = PCI_DEVICE_ID_VIA_VL805;
    k->revision = 0x01;
    dc->desc = "VIA VL805 xHCI (Raspberry Pi 4B)";
}

static const TypeInfo vl805_xhci_info = {
    .name          = TYPE_VL805_XHCI,
    .parent        = TYPE_XHCI_PCI,
    .instance_init = vl805_xhci_instance_init,
    .class_init    = vl805_xhci_class_init,
};

static void vl805_xhci_register_types(void)
{
    type_register_static(&vl805_xhci_info);
}

type_init(vl805_xhci_register_types)
