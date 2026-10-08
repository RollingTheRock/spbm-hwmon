obj-m += spbm.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MOK_KEY ?= /var/lib/shim-signed/mok/MOK.priv
MOK_CERT ?= /var/lib/shim-signed/mok/MOK.der
SIGN_FILE ?= $(KDIR)/scripts/sign-file

all: modules sign

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

sign: modules
	@if [ -f "$(MOK_KEY)" ] && [ -f "$(MOK_CERT)" ]; then \
		sudo $(SIGN_FILE) sha256 $(MOK_KEY) $(MOK_CERT) $(CURDIR)/spbm.ko && \
		echo "Signed spbm.ko"; \
	else \
		echo "MOK keys not found, skipping signing"; \
	fi

load: all
	-sudo rmmod spbm 2>/dev/null || true
	sudo insmod ./spbm.ko
	@sleep 0.5
	sudo dmesg | grep "spbm" | tail -10
	@echo "---"
	@sensors spbm-* 2>/dev/null || \
		(echo "sensors not found, reading sysfs directly:" && \
		 for f in /sys/class/hwmon/hwmon*/name; do \
			if [ "$$(cat $$f 2>/dev/null)" = "spbm" ]; then \
				d=$$(dirname $$f); \
				for p in $$d/power*_label; do \
					[ -f "$$p" ] && echo "$$(cat $$p): $$(cat $${p%_label}_input) uW"; \
				done; \
				for e in $$d/energy*_label; do \
					[ -f "$$e" ] && echo "$$(cat $$e): $$(cat $${e%_label}_input) uJ"; \
				done; \
			fi; \
		done)

unload:
	-sudo rmmod spbm 2>/dev/null

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules sign load unload clean
