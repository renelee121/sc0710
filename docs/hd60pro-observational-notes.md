# Elgato HD60 Pro observational backend

Device:

- PCI ID: `12ab:0380`
- Subsystem: `1cfa:0006`
- BAR0 size: `0x100000`
- BAR5 size: `0x1000`

Current safety state:

- PCI memory space enabled
- Bus mastering disabled
- IRQ disabled
- DMA disabled
- MMIO writes disabled
- MMIO reads restricted to whitelist

Observed read-only registers:

- BAR0 `+0x30`: `0x00000000`
- BAR0 `+0x40`: `0x00000000`

Runtime validation:

- Module loaded successfully on CachyOS kernel `7.1.5-1-cachyos`
- No new AMD-Vi `IO_PAGE_FAULT` messages
- Device remained `BusMaster-`

Windows driver observations:

- Resource 0 is used for offsets `0x00`, `0x30`, `0x40`, and `0x50`
- Resource 1 is used for offset `0xdc`
- `+0x30` is read and then cleared with a separate write
- `+0x40` is read and masked with `0x7`

Open questions:

- Confirm translated Windows resource index 0 maps to PCI BAR0
- Identify additional read-only registers
- Determine reset and DMA initialization sequence
