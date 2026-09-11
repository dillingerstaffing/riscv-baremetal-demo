# menvcfg-stce

Probes `menvcfg.STCE` (bit 63), the machine-mode advertisement of the
Sstc extension: reads `menvcfg`, exercises the bit's WARL behavior
(clear it, write all-ones, restore the boot value), then drops to
S-mode and checks whether real `stimecmp` access agrees with the
advertisement. When the advertisement is honest, it arms a supervisor
timer interrupt from S-mode and requires exactly one delegated trap
with a quiet window showing no re-delivery.
