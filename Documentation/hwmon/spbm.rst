.. SPDX-License-Identifier: GPL-2.0

Kernel driver spbm
==================

Supported chips:

  * NVIDIA DGX Spark (GB10 SoC)
    Prefix: 'spbm'
    Addresses: ACPI NVDA8800 (\_SB_.MTEL)
    Datasheet: Reverse-engineered from ACPI DSDT _DSM method

Author: Andrew Wang <RollingTheRock>

Description
-----------

The System Power Budget Manager (SPBM) driver exposes power, energy, and
temperature telemetry for NVIDIA DGX Spark systems equipped with the GB10 SoC.

On DGX Spark systems, power management firmware running on the MediaTek SSPM
continuously monitors power delivery rails, accumulators, and thermal zones,
writing live telemetry into a shared SRAM region known as the SPBM memory buffer.

The driver binds to the ACPI device ``NVDA8800`` (located at ``\_SB_.MTEL``).
At probe time, it uses the device's ``_DSM`` (UUID ``12345678-1234-1234-1234-56789abc``):

1. Function 1 discovers the index of the ``SPBM`` memory resource in ``_CRS``.
2. Function 2 discovers register offsets by matching canonical symbolic names
   (e.g., ``SPBM_TE_SYS_TOTAL_TELEMETRY_OFFSET``), allowing resilience across
   firmware updates.

DMI Whitelist Protection
------------------------

To prevent false loading on non-GB10 ARM64 systems that might expose the same
generic ACPI HID, the driver verifies platform identification using DMI matching
before performing MMIO mapping. Supported platforms include:

- System Vendor: ``NVIDIA`` or ``XFUSION``
- Product Name / Family: ``DGX Spark``, ``FusionXpark GB10``

Module Parameters
-----------------

* ``force``: bool (default: false)
  Force driver load even if the platform is not recognized by the DMI whitelist table.
  Enables operation on engineering samples and third-party GB10 hardware.


Sysfs Attributes
----------------

All telemetry attributes adhere to the standard Linux hwmon ABI
(``Documentation/hwmon/sysfs-interface.rst``).

Power Telemetry (uW)
~~~~~~~~~~~~~~~~~~~~

The driver exposes 14 power channels in microwatts:

================  ===============  =========================================
Channel           Attribute        Description
================  ===============  =========================================
power1_input      sys_total        Total system power consumption
power2_input      soc_pkg          SoC package power
power3_input      cpu_gpu          Combined CPU and GPU power
power4_input      cpu_p            Cortex-X925 Performance core cluster power
power5_input      cpu_e            Cortex-A725 Efficiency core cluster power
power6_input      vcore            Core voltage domain power
power7_input      dc_input         DC input / charger rail power
power8_input      gpu              Discrete GPU domain power
power9_input      prereg           Pre-regulator input power
power10_input     dla              Deep Learning Accelerator power
power11_input     pl1              EWMA smoothed power seen by PL1 controller
power12_input     pl2              EWMA smoothed power seen by PL2 controller
power13_input     syspl1           EWMA smoothed power seen by SysPL1
power14_input     syspl2           EWMA smoothed power seen by SysPL2
================  ===============  =========================================

Power Limit Controls (uW)
~~~~~~~~~~~~~~~~~~~~~~~~~

Channels 11 through 14 (``pl1``, ``pl2``, ``syspl1``, ``syspl2``) provide power
limit control via standard attributes:

- ``power[11-14]_cap``: Effective power limit (read/write). Writing 0 resets to EC default.
- ``power[11-14]_max``: Firmware ceiling limit (read-only).
- ``power[11-14]_min``: Firmware floor limit (read-only).

All write operations are serialized through a kernel mutex to ensure atomicity
with firmware handshake registers.

Energy Accumulators (uJ)
~~~~~~~~~~~~~~~~~~~~~~~~

The driver maintains 64-bit monotonically unwrapped energy accumulators in microjoules:

================  ===============  =========================================
Channel           Attribute        Description
================  ===============  =========================================
energy1_input     pkg              Cumulative SoC package energy
energy2_input     cpu_e            Cumulative CPU E-core energy
energy3_input     cpu_p            Cumulative CPU P-core energy
energy4_input     gpu              Cumulative GPU energy
================  ===============  =========================================

The underlying firmware counters are 32-bit millijoule registers. The driver
handles 32-bit roll-over arithmetic internally, providing monotonically increasing
64-bit microjoule counters suitable for continuous Prometheus and APM scraping.
To guard against MediaTek SSPM firmware resets and erratic counter drops, single-step
energy deltas are validated against a physical ceiling (1.8 GJ, representing 1 hour
at 500W peak power). Counter discontinuities exceeding this bound trigger automatic
baseline re-synchronization without phantom energy jumps.


Temperature Sensors (millidegrees C)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The driver exposes 8 thermal zones in millidegrees Celsius:

================  ===============  =========================================
Channel           Attribute        Description
================  ===============  =========================================
temp1_input       tj_max           Package maximum temperature
temp2_input       cpu_e_clu0       E-core cluster 0 temperature
temp3_input       cpu_p_clu0       P-core cluster 0 temperature
temp4_input       cpu_e_clu1       E-core cluster 1 temperature
temp5_input       cpu_p_clu1       P-core cluster 1 temperature
temp6_input       gpu              GPU thermal zone temperature
temp7_input       soc              SoC thermal zone temperature
temp8_input       dla              DLA thermal zone temperature
================  ===============  =========================================

Thermal Throttling Alarm:

- ``temp1_crit_alarm``: Set to 1 when the PROCHOT thermal throttle signal is
  active, 0 during normal operation.

Architecture & Planned Evolution
--------------------------------

The driver currently binds as an ``acpi_driver`` to ``NVDA8800`` because the
OEM ACPI DSDT omits standard ``_UID`` and ``_STA`` methods, preventing the
kernel ACPI bus from automatically instantiating a ``platform_device``.

Planned Evolution (Andrew Wang / ##RollingTheRock):
Future iterations will introduce a self-instantiating adapter utilizing
``acpi_create_platform_device()``, refactoring the driver core into a pure,
modern ``platform_driver`` conforming to the standard Linux Device Model (LDM).

Acknowledgments
---------------

Special thanks to Antheas Kapenekakis for the initial reverse-engineering prototype
implementation (spark_hwmon), which served as the foundational draft and inspired
this industrial-grade driver refactoring.
