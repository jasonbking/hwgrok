# hwgrok

`hwgrok` distills an illumos topo snapshot and SMBIOS data into a versioned
JSON hardware inventory for higher-level management software.

## Build instructions

`hwgrok` must be built on an illumos-based system. It has been successfully
built on OpenIndiana and in a SmartOS zone. The makefile uses GNU make syntax,
so use `gmake`, not `/usr/ccs/bin/make`.

```text
gmake all
```

The makefile expects to find GCC in `PATH`. Override `CC` when necessary:

```text
gmake CC=/opt/local/bin/gcc all
```

To build against an illumos proto area instead of the installed headers and
libraries:

```text
gmake PROTO=/ws/my-illumos-ws/proto all
```

## Output format

The top-level object contains:

- `schema_version`: output schema version, currently `1`.
- `system`: SMBIOS Type 1 identity and topo motherboard information.
- `cpus`: processor model, label, serial number, core count, and thread count.
- `dimms`: label, byte size, manufacturer, product, part, and serial number.
- `network_interfaces`: physical NIC ports, including their driver name,
  instance, primary MAC address, PCI identity, location, and transceiver.
- `fibre_channel_hbas`: FC ports with their `cN` device name, adapter
  manufacturer, model, serial number, port WWN, and optional transceiver/UFM
  information.
- `slots`: SMBIOS Type 9 slots and their topo-correlated occupants.

UFM data is included as an optional `ufm` array on devices that expose UFM
topology children. Each UFM image contains a description and its firmware
slots, including version, active state, and read/write mode.

NVMe controllers contain a `namespaces` array. The namespace `name` is its
logical disk name, such as `c0t12345d0`; capacity and logical block size are
reported as byte counts.

Properties that are unavailable are omitted. `null` is used for a known
absence, such as an unoccupied physical slot.

Example:

```json
{
  "schema_version": 1,
  "system": {
    "serial_number": "SYS123456",
    "description": "Supermicro SYS-1029U",
    "manufacturer": "Supermicro",
    "product": "SYS-1029U",
    "motherboard": {
      "id": "hc:///motherboard=0",
      "manufacturer": "Supermicro",
      "model": "X11DPH-T",
      "serial_number": "MB123456",
      "revision": "1.01"
    }
  },
  "cpus": [
    {
      "id": "hc:///motherboard=0/chip=0",
      "label": "CPU1",
      "model": "Intel(R) Xeon(R) Gold 6230 CPU @ 2.10GHz",
      "serial_number": "0000000000000001",
      "cores": 20,
      "threads": 40
    }
  ],
  "dimms": [
    {
      "id": "hc:///motherboard=0/memory-controller=0/dimm=0",
      "label": "P1-DIMMA1",
      "size_bytes": 34359738368,
      "manufacturer": "Samsung",
      "serial_number": "1234ABCD",
      "part_number": "M393A4K40CB2-CTD"
    }
  ],
  "network_interfaces": [
    {
      "id": "hc:///motherboard=0/pciexbus=3/pciexdev=0/pciexfn=0/port=0",
      "name": "ixgbe0",
      "driver": "ixgbe",
      "instance": 0,
      "manufacturer": "Intel",
      "model": "Intel X520 10GbE SFP+",
      "primary_mac_address": "00:1b:21:aa:bb:cc",
      "location": {
        "type": "pci-slot",
        "slot": "PCIe Slot 3"
      },
      "transceiver": {
        "type": "sff",
        "usable": true,
        "vendor": "FINISAR CORP.",
        "part_number": "FTLX8571D3BCL",
        "revision": "A",
        "serial_number": "FNS12345678"
      }
    }
  ],
  "fibre_channel_hbas": [
    {
      "id": "hc:///motherboard=0/pciexbus=5/pciexdev=0/pciexfn=0/port=0",
      "name": "c4",
      "driver": "fp",
      "instance": 4,
      "manufacturer": "QLogic Corp.",
      "model": "ISP2722-based 16/32Gb Fibre Channel Adapter",
      "serial_number": "RFD1234",
      "port_wwn": "21000024ff123456",
      "node_wwn": "20000024ff123456"
    }
  ],
  "slots": [
    {
      "id": "smbios-slot:0:5:0",
      "label": "PCIe Slot 5",
      "occupied": true,
      "occupant": {
        "type": "nvme",
        "name": "nvme2",
        "driver": "nvme",
        "instance": 2,
        "vendor": "Samsung",
        "product": "MZQLB3T8HALS-00007",
        "serial_number": "S4C9NE0M123456",
        "firmware_revision": "EDA7402Q",
        "nvme_version": "1.4",
        "capacity_bytes": 3840755982336,
        "namespaces": [
          {
            "namespace_id": 1,
            "name": "c0t12345d0",
            "vendor": "Samsung",
            "product": "MZQLB3T8HALS-00007",
            "serial_number": "S4C9NE0M123456",
            "size_bytes": 3840755982336,
            "logical_block_size": 512
          }
        ],
        "ufm": [
          {
            "description": "NVMe Controller Firmware",
            "slots": [
              {
                "id": 1,
                "version": "EDA7402Q",
                "active": true,
                "mode": "read-write"
              }
            ]
          }
        ]
      }
    }
  ]
}
```
