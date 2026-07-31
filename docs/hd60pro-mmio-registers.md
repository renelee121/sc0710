# Elgato HD60 Pro MMIO register observations

Hardware:

- PCI device: `12ab:0380`
- Subsystem: `1cfa:0006`
- BAR0 size: `0x100000`
- BAR5 size: `0x1000`

## Safety rules

- No blind BAR scanning.
- No MMIO writes.
- Add an offset to the Linux whitelist only after confirming:
  - BAR number
  - access width
  - read operation
  - calling context
  - supporting static or dynamic evidence

## Confirmed non-MMIO fields

| Offset | Meaning | Notes |
|---|---|---|
| `device + 0x69d0` | command-completion semaphore/state | This is an offset inside the Windows device context, not a BAR offset. Do not add to the MMIO whitelist. |

## Interrupt observations

- ISR: `0x14028ec70`
- DPC: `0x14028ee50`
- IRQ bit 11 appears associated with command completion.
- Bit 11 is not currently confirmed as frame-ready.

## Candidate MMIO accesses

| BAR | Offset | Width | Access | Function | Context | Confidence | Evidence |
|---:|---:|---:|---|---|---|---|---|
