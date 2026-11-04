# USB4 and Thunderbolt

air/OS drives USB4 hosts (and Thunderbolt 3/4 devices connected to them) with
its own software connection manager: the `usb4` kernel add-on in
`src/add-ons/kernel/busses/usb/usb4`. It attaches to every USB4 host interface
(PCI class 0c/03/40), so it covers AMD Ryzen USB4 (Rembrandt and later,
Phoenix, Strix, Krackan), Intel Meteor Lake and later, and Intel Ice Lake to
Raptor Lake whenever the firmware hands the operating system native USB4
control through ACPI `_OSC`.

## What it does

- **Host interface**: maps the host interface, runs the control channel on
  ring 0 (MSI, MSI-X, legacy or polled), CRC-32C framed control packets,
  hot plug and notification packets.
- **Routers**: enumerates the host router and every router below it (docks,
  hubs, enclosures, displays), gives them their route, reads their buffer
  allocation parameters and DROM names, bonds the two lanes of a link,
  starts the time management unit (needed for DisplayPort), marks the
  configuration valid.
- **Tunnels**:
  - *USB3*: from the host's USB3 down adapter to the device router's USB3 up
    adapter, with the host USB3 bandwidth allocated and given back to
    DisplayPort when a monitor needs it. The USB devices behind it show up on
    the USB4-attached xHCI controller as SuperSpeed devices.
  - *DisplayPort*: every DP OUT adapter with a monitor gets a free DP IN of
    the host. The capabilities of both ends are exchanged and the link rate
    reduced to what the USB4 link has left. The graphics driver (amdgpu's
    DPIA links) then sees the monitor as a normal hot plug.
  - *PCIe*: from the host's PCIe down adapter to the device's PCIe up adapter.
    The PCI bus manager's hot plug module then numbers, sizes and publishes
    the PCI devices that appear behind the PCIe root port (NVMe enclosures,
    network adapters, external GPUs, the PCIe switch of a dock).
- **Hot plug**: plugging a device in or out of any USB4 port, chained ones
  too, sets up or tears down its tunnels; monitors plugged into a dock come
  and go the same way.
- **Boot firmware tunnels**: the firmware may have set up tunnels before the
  system started. The connection manager resets the host router's USB4
  ports (USB4 version 1 hosts) or the whole host router (version 2) and
  builds everything again.

Not done yet: host-to-host (XDomain) networking, the firmware connection
manager (ICM) of Intel Thunderbolt 3 controllers and of Intel hosts whose
firmware keeps control, router NVM updates, CL low power link states, the
DisplayPort bandwidth allocation mode, and sleep.

## Using it

`/dev/bus/usb4/<n>` (one per host interface) shows the routers, their
adapters and the tunnels:

    cat /dev/bus/usb4/1

and takes commands:

| command          | effect                                                  |
|------------------|---------------------------------------------------------|
| `start`, `stop`  | start or stop the connection manager                    |
| `rescan`         | look at every port again, set up missing tunnels        |
| `allow <kind>`, `deny <kind>` | allow or forbid `usb3`, `dp`, `pcie` or `all` tunnels |
| `debug <level>`  | 0 quiet, 1 progress, 2 every packet and hop             |

    echo "deny pcie" > /dev/bus/usb4/1

Driver settings (`~/config/settings/kernel/drivers/usb4`):

    auto_start true         # start the connection manager at boot
    usb3_tunneling true
    dp_tunneling true
    pcie_tunneling true
    debug 0

Every tunnel kind is also subject to what the platform granted through
`_OSC`. The syslog lines start with `usb4:` and `PCI: hot plug:`.

## Code

| file          | contents                                                 |
|---------------|----------------------------------------------------------|
| `Nhi.cpp`     | host interface, rings, interrupts, control packets       |
| `Router.cpp`  | routers and adapters, link setup, router operations, TMU |
| `Tunnel.cpp`  | paths (hop entries along the route), the three tunnels   |
| `Domain.cpp`  | the connection manager: events, scanning, bandwidth      |
| `usb4.cpp`    | device manager glue, `_OSC`, settings, devfs node        |
| `../../bus_managers/pci/pci_hotplug.cpp` | PCI Express hot plug    |

The register layout follows the USB4 specification; the behaviour follows the
USB4 connection manager guide and was checked against the Linux driver.
