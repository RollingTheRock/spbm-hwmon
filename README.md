# SPBM - NVIDIA DGX Spark Power Telemetry & Control hwmon Driver

Linux kernel hardware monitoring (`hwmon`) driver for the NVIDIA DGX Spark (GB10 SoC)
that exposes full system power telemetry, energy accumulators, thermal zones, and
power limit controls via standard Linux `sensors` and `sysfs` interfaces.

The driver interfaces with the System Power Budget Manager (SPBM) shared memory buffer,
which is continuously updated by MediaTek SSPM power management firmware with live
readings in milliwatts, cumulative energy counters in millijoules, and thermal zone
temperatures in centidegrees Celsius. Writable `power_cap` attributes allow setting
OS-level power limits (PL1, PL2, SysPL1, SysPL2).

> [!NOTE]
> NVIDIA has [officially stated](https://forums.developer.nvidia.com/t/help-needed-how-to-enable-grace-cpu-power-telemetry-on-dgx-spark-gb10/360631)
> that there is "no method to monitor CPU power" on the DGX Spark. This driver solves that
> gap by communicating with the underlying ACPI SPBM firmware buffer.

---

## Key Architectural Highlights

- **Hardware Whitelist (DMI Quirk)**: Uses `dmi_check_system()` to verify the system vendor (`NVIDIA`, `XFUSION`) and product family (`DGX Spark`, `FusionXpark GB10`), safely ignoring non-target ARM64 platforms.
- **Dynamic DSM Resolution & Bounds Defense**: Dynamically discovers memory indices via `_DSM` Function 1 and maps canonical register names via Function 2, enforcing boundary checks against ACPI `_CRS` memory allocation to prevent MMIO overruns.
- **64-bit Monotonic Energy Unwrapping**: Unwraps 32-bit millijoule hardware counters into strictly monotonic 64-bit microjoule accumulators, eliminating roll-over data cliffs in monitoring systems.
- **Mutex Concurrency Protection**: Serializes all power limit writes and firmware handshake updates through a dedicated kernel mutex.
- **Standard hwmon ABI**: Strictly conforms to `Documentation/hwmon/sysfs-interface.rst`. Thermal throttling (PROCHOT) is mapped to standard `temp1_crit_alarm`.

---

## Sensors

### Power (14 channels)

| Channel | Label | Idle (Typical) | Description |
|---------|-------|----------------|-------------|
| power1  | sys_total | ~25 W | Total system power consumption |
| power2  | soc_pkg   | ~17 W | SoC package power |
| power3  | cpu_gpu   | ~6 W  | Combined CPU and discrete GPU power |
| power4  | cpu_p     | ~0.5 W | P-core cluster (10x Cortex-X925) |
| power5  | cpu_e     | ~0.01 W | E-core cluster (10x Cortex-A725) |
| power6  | vcore     | ~4 W  | Core voltage rail power |
| power7  | dc_input  | ~26 W | DC input / power supply rail |
| power8  | gpu       | ~5 W  | Discrete GPU power |
| power9  | prereg    | ~8 W  | Pre-regulator input power |
| power10 | dla       | ~0.1 W | Deep Learning Accelerator power |
| power11 | pl1       | ~18 W | EWMA power seen by PL1 controller (cap=rw, max=250W) |
| power12 | pl2       | ~18 W | EWMA power seen by PL2 controller (cap=rw, max=250W) |
| power13 | syspl1    | ~26 W | EWMA power seen by SysPL1 controller (cap=rw, max=300W) |
| power14 | syspl2    | ~27 W | EWMA power seen by SysPL2 controller (cap=rw, max=300W) |

### Energy (4 accumulators)

| Channel | Label | Unit | Description |
|---------|-------|------|-------------|
| energy1 | pkg   | uJ   | Cumulative SoC package energy |
| energy2 | cpu_e | uJ   | Cumulative CPU E-core energy |
| energy3 | cpu_p | uJ   | Cumulative CPU P-core energy |
| energy4 | gpu   | uJ   | Cumulative GPU energy |

Energy accumulators provide 64-bit monotonically unwrapped microjoule readings suitable for continuous Prometheus and APM scraping.

### Temperature (8 thermal zones)

| Channel | Label | Typical | Description |
|---------|-------|---------|-------------|
| temp1   | tj_max     | ~31 °C | Package junction maximum temperature |
| temp2   | cpu_e_clu0 | ~31 °C | E-core cluster 0 temperature |
| temp3   | cpu_p_clu0 | ~31 °C | P-core cluster 0 temperature |
| temp4   | cpu_e_clu1 | ~31 °C | E-core cluster 1 temperature |
| temp5   | cpu_p_clu1 | ~31 °C | P-core cluster 1 temperature |
| temp6   | gpu        | ~31 °C | Discrete GPU thermal zone |
| temp7   | soc        | ~31 °C | SoC thermal zone |
| temp8   | dla        | ~27 °C | Deep Learning Accelerator thermal zone |

- `temp1_crit_alarm`: Exposes the PROCHOT thermal throttle status (`1` when throttled, `0` under normal operation).

---

## Power Limit Control (`power_cap`)

The `pl1`, `pl2`, `syspl1`, and `syspl2` channels expose standard hwmon attributes:
- `power[11-14]_cap`: Read/write effective power limit in microwatts.
- `power[11-14]_max`: Read-only hardware ceiling limit.
- `power[11-14]_min`: Read-only hardware floor limit.

Writes outside the `[power_min, power_max]` range return `-EINVAL`. Write `0` to reset to the firmware default.

Example: Set sustained PL1 limit to 100W:
```bash
echo 100000000 | sudo tee /sys/class/hwmon/hwmonN/power11_cap
```

Reset to firmware default:
```bash
echo 0 | sudo tee /sys/class/hwmon/hwmonN/power11_cap
```

---

## Installation via DKMS

```bash
sudo apt install dkms
sudo dkms add .
sudo dkms build spbm/0.3.0
sudo dkms install spbm/0.3.0
```

The module auto-loads at boot via ACPI modalias matching (`NVDA8800`). DKMS will automatically rebuild the driver on kernel upgrades.

To uninstall:
```bash
sudo dkms remove spbm/0.3.0 --all
```

---

## Manual Build & Test

```bash
# Run unit tests locally
make test

# Build kernel module (requires linux-headers)
make

# Build, load, and inspect sensors
sudo make load

# Unload kernel module
make unload
```

---

## Roadmap: Self-Instantiating platform_driver Abstraction

Currently, the driver binds as an `acpi_driver` to `NVDA8800` because the OEM ACPI DSDT omits standard `_UID` and `_STA` objects, causing the Linux ACPI core to bypass automatic `platform_device` instantiation.

**Planned Evolution (Andrew Wang / ##RollingTheRock):**
In the next milestone, we will introduce a lightweight self-instantiating ACPI-to-platform adapter using the kernel-exported `acpi_create_platform_device()` symbol. This will:
1. Automatically bind to the `NVDA8800` ACPI handle upon module initialization.
2. Instantiate a first-class `struct platform_device` on `platform_bus_type`.
3. Fully refactor the driver core into a clean, modern `platform_driver` (`platform_driver_register`, `platform_get_resource`, `devm_ioremap_resource`), aligning 100% with the standard Linux Device Model (LDM) while preserving standalone out-of-tree agility.

---

## Author & Maintainer

- **Author**: Andrew Wang ([@RollingTheRock](https://github.com/RollingTheRock)) `##RollingTheRock`

---

## Acknowledgments

Special thanks to **Antheas Kapenekakis** for the initial reverse-engineering prototype implementation (`spark_hwmon`), which served as the foundational draft and inspired this industrial-grade driver refactoring.

---

## License

This project is licensed under the **GNU General Public License v2.0 (GPL-2.0)**, consistent with the Linux kernel itself. See [LICENSE](LICENSE) for details.

