# Linux Kernel in-tree integration snippets for SPBM driver

## 1. drivers/hwmon/Kconfig
Add the following block to drivers/hwmon/Kconfig (alphabetically ordered under NVIDIA section):

```kconfig
config SENSORS_SPBM
	tristate "NVIDIA DGX Spark (GB10) SPBM hardware monitoring"
	depends on ACPI && (ARM64 || COMPILE_TEST)
	help
	  If you say yes here you get support for the System Power Budget
	  Manager (SPBM) shared memory power and thermal telemetry interface
	  found on NVIDIA DGX Spark (GB10 SoC) systems.

	  This driver can also be built as a module. If so, the module
	  will be called spbm.
```

## 2. drivers/hwmon/Makefile
Add the following line to drivers/hwmon/Makefile:

```makefile
obj-$(CONFIG_SENSORS_SPBM)	+= spbm.o
```

## 3. MAINTAINERS
Add the following entry to the kernel MAINTAINERS file:

```text
SPBM HARDWARE MONITORING DRIVER
M:	Antheas Kapenekakis <antheas@cs.aau.dk>
L:	linux-hwmon@vger.kernel.org
S:	Maintained
F:	Documentation/hwmon/spbm.rst
F:	drivers/hwmon/spbm.c
F:	drivers/hwmon/spbm_core.h
```
