# spbm-hwmon

Linux hardware monitoring (`hwmon`) driver for NVIDIA DGX Spark (GB10 SoC). Exposes system power telemetry, energy counters, thermal zones, and power limit controls via standard sysfs interfaces.

Interfaces with the System Power Budget Manager (SPBM) shared memory buffer updated by MediaTek SSPM power management firmware.

---

## Features

- **Standard hwmon ABI**: Fully compatible with `sensors` and standard monitoring tools. Thermal throttling (PROCHOT) exposed as `temp1_crit_alarm`.
- **Hardware Whitelist & Force Override**: DMI quirk filtering (`dmi_check_system`) for target platforms (`NVIDIA DGX Spark`, `FusionXpark GB10`), with `force=1` module parameter override for unlisted hardware.
- **Dynamic DSM & Bounds Defense**: Discovers memory indices and canonical names via `_DSM`, validated against ACPI resource lengths.
- **64-bit Monotonic Energy with Reset Protection**: Unwraps 32-bit hardware millijoule counters into monotonic 64-bit microjoule accumulators, with anomaly bounds protection against firmware restarts.
- **Concurrency Protection**: Mutex-serialized mailbox updates and power limit writes.


---

## Channels

### Power (14 channels)

| Channel | Label | Typical (Idle) | Description |
|---------|-------|----------------|-------------|
| power1  | sys_total | ~25 W | Total system power |
| power2  | soc_pkg   | ~17 W | SoC package power |
| power3  | cpu_gpu   | ~6 W  | Combined CPU and discrete GPU |
| power4  | cpu_p     | ~0.5 W | P-core cluster (10x Cortex-X925) |
| power5  | cpu_e     | ~0.01 W | E-core cluster (10x Cortex-A725) |
| power6  | vcore     | ~4 W  | Core voltage rail |
| power7  | dc_input  | ~26 W | DC input rail |
| power8  | gpu       | ~5 W  | Discrete GPU |
| power9  | prereg    | ~8 W  | Pre-regulator input |
| power10 | dla       | ~0.1 W | Deep Learning Accelerator |
| power11 | pl1       | ~18 W | PL1 EWMA power (cap=rw, max=250W) |
| power12 | pl2       | ~18 W | PL2 EWMA power (cap=rw, max=250W) |
| power13 | syspl1    | ~26 W | SysPL1 EWMA power (cap=rw, max=300W) |
| power14 | syspl2    | ~27 W | SysPL2 EWMA power (cap=rw, max=300W) |

### Energy (4 accumulators)

| Channel | Label | Unit | Description |
|---------|-------|------|-------------|
| energy1 | pkg   | uJ   | Cumulative SoC package energy |
| energy2 | cpu_e | uJ   | Cumulative CPU E-core energy |
| energy3 | cpu_p | uJ   | Cumulative CPU P-core energy |
| energy4 | gpu   | uJ   | Cumulative GPU energy |

### Temperature (8 thermal zones)

| Channel | Label | Typical | Description |
|---------|-------|---------|-------------|
| temp1   | tj_max     | ~31 °C | Package junction max temp |
| temp2   | cpu_e_clu0 | ~31 °C | E-core cluster 0 |
| temp3   | cpu_p_clu0 | ~31 °C | P-core cluster 0 |
| temp4   | cpu_e_clu1 | ~31 °C | E-core cluster 1 |
| temp5   | cpu_p_clu1 | ~31 °C | P-core cluster 1 |
| temp6   | gpu        | ~31 °C | Discrete GPU |
| temp7   | soc        | ~31 °C | SoC thermal zone |
| temp8   | dla        | ~27 °C | Deep Learning Accelerator |

- `temp1_crit_alarm`: PROCHOT thermal throttle indicator (`1` = throttled, `0` = normal).

---

## Power Limit Control

Channels `pl1`, `pl2`, `syspl1`, `syspl2` expose standard limit attributes:
- `power[11-14]_cap`: Effective limit in microwatts (read/write).
- `power[11-14]_max` / `min`: Hardware limits (read-only).

Set PL1 limit to 100W:
```bash
echo 100000000 | sudo tee /sys/class/hwmon/hwmonN/power11_cap
```

Reset to firmware default:
```bash
echo 0 | sudo tee /sys/class/hwmon/hwmonN/power11_cap
```

---

## Installation

### DKMS

```bash
sudo apt install dkms
sudo dkms add .
sudo dkms build spbm/0.3.0
sudo dkms install spbm/0.3.0
```

To remove:
```bash
sudo dkms remove spbm/0.3.0 --all
```

### Manual Build

```bash
make
sudo make load
sudo make unload
```

Run test suite:
```bash
make test
```

---

## Integration with Aitra Meter

`spbm-hwmon` is fully compatible with [Aitra Meter](https://github.com/aitra-ai/aitra-meter) host energy monitoring out-of-the-box (`--host-energy-provider=grace-spark-hwmon`). Its 64-bit monotonic accumulators resolve the 32-bit firmware rollover cliff, enabling continuous long-term Prometheus telemetry on NVIDIA GB10 / DGX Spark systems.

---

## Roadmap

- Migrate driver model from `acpi_driver` to `platform_driver` via `acpi_create_platform_device()`.

---

## Author

Andrew Wang ([@RollingTheRock](https://github.com/RollingTheRock))

---

## Acknowledgments

Based on initial reverse-engineering work from `spark_hwmon` by Antheas Kapenekakis.

---

## License

GPL-2.0
