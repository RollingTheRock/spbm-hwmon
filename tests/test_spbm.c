/*
 * Unit tests for spbm driver core functions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>

#include "../spbm_core.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		printf("[-] FAILED: %s (at line %d): %s\n", __func__, __LINE__, msg); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define TEST_PASS() printf("[+] PASSED: %s\n", __func__)

/* --- Stage 1 Tests: DMI Match & Memory Bounds --- */

static void test_dmi_match_valid_xfusion(void)
{
	struct spbm_dmi_info dmi = {
		.sys_vendor = "XFUSION",
		.product_name = "FusionXpark GB10",
		.product_family = "DGX Spark",
	};
	TEST_ASSERT(spbm_dmi_is_supported(&dmi) == true,
		    "XFUSION FusionXpark GB10 should be supported");
	TEST_PASS();
}

static void test_dmi_match_valid_nvidia(void)
{
	struct spbm_dmi_info dmi = {
		.sys_vendor = "NVIDIA",
		.product_name = "DGX Spark",
		.product_family = "DGX Spark",
	};
	TEST_ASSERT(spbm_dmi_is_supported(&dmi) == true,
		    "NVIDIA DGX Spark should be supported");
	TEST_PASS();
}

static void test_dmi_reject_foreign_hardware(void)
{
	struct spbm_dmi_info dmi1 = {
		.sys_vendor = "QEMU",
		.product_name = "Standard PC",
		.product_family = "",
	};
	TEST_ASSERT(spbm_dmi_is_supported(&dmi1) == false,
		    "QEMU should be rejected");

	struct spbm_dmi_info dmi2 = {
		.sys_vendor = "Dell Inc.",
		.product_name = "PowerEdge R750",
		.product_family = "",
	};
	TEST_ASSERT(spbm_dmi_is_supported(&dmi2) == false,
		    "Dell server should be rejected");
	TEST_PASS();
}

static void test_bounds_valid_range(void)
{
	uint64_t res_size = 0x1000;
	TEST_ASSERT(spbm_validate_bounds(0x0, 4, res_size) == 0,
		    "offset 0 with size 4 should be valid");
	TEST_ASSERT(spbm_validate_bounds(0xFFC, 4, res_size) == 0,
		    "offset 0xFFC with size 4 should be valid in 4KB");
	TEST_PASS();
}

static void test_bounds_out_of_range(void)
{
	uint64_t res_size = 0x1000;
	TEST_ASSERT(spbm_validate_bounds(0xFFD, 4, res_size) == -ERANGE,
		    "offset 0xFFD with size 4 should exceed 4KB range");
	TEST_ASSERT(spbm_validate_bounds(0x2000, 4, res_size) == -ERANGE,
		    "offset 0x2000 should exceed 4KB range");
	TEST_ASSERT(spbm_validate_bounds(UINT64_MAX - 2, 4, res_size) == -EOVERFLOW,
		    "wrapping offset should return EOVERFLOW");
	TEST_PASS();
}

/* --- Stage 2 Tests: Mutex & Mailbox Concurrency --- */

struct thread_arg {
	struct spbm_mock_device *dev;
	int count;
};

static void *worker_func(void *arg)
{
	struct thread_arg *ta = (struct thread_arg *)arg;
	for (int i = 0; i < ta->count; i++) {
		spbm_mock_write_power_cap(ta->dev, 0, (long)(i + 1) * 1000);
	}
	return NULL;
}

static void test_mailbox_serialization(void)
{
	struct spbm_mock_device dev;
	spbm_mock_device_init(&dev);

	int ret = spbm_mock_write_power_cap(&dev, 0, 150000000);
	TEST_ASSERT(ret == 0, "mock write should succeed");
	TEST_ASSERT(dev.last_val == 150000, "val should be converted to mW (150000)");
	TEST_ASSERT(dev.poke_count == 1, "poke should be triggered once");
	TEST_ASSERT(dev.in_critical_section == false, "device should not remain locked");

	/* Test concurrent multi-thread writes */
#define N_TEST_THREADS 4
	const int writes_per_thread = 1000;
	pthread_t threads[N_TEST_THREADS];
	struct thread_arg args[N_TEST_THREADS];

	for (int i = 0; i < N_TEST_THREADS; i++) {
		args[i].dev = &dev;
		args[i].count = writes_per_thread;
		pthread_create(&threads[i], NULL, worker_func, &args[i]);
	}

	for (int i = 0; i < N_TEST_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	TEST_ASSERT(dev.poke_count == (uint32_t)(1 + N_TEST_THREADS * writes_per_thread),
		    "all concurrent pokes must be recorded without lost updates");
#undef N_TEST_THREADS
	TEST_PASS();
}

/* --- Stage 3 Tests: Monotonic Energy Unwrapping --- */

static void test_energy_monotonic_unwrapping(void)
{
	struct spbm_energy_acc acc;
	spbm_energy_acc_init(&acc, 0);

	/* Sample 1: initial reading */
	uint64_t val1 = spbm_energy_acc_update(&acc, 0x1000);
	TEST_ASSERT(val1 == (uint64_t)0x1000 * 1000, "val1 should be 0x1000 * 1000 uJ");

	/* Sample 2: near 32-bit ceiling */
	uint64_t val2 = spbm_energy_acc_update(&acc, 0xFFFFFF00);
	TEST_ASSERT(val2 == (uint64_t)0xFFFFFF00 * 1000, "val2 should be 0xFFFFFF00 * 1000 uJ");

	/* Sample 3: 32-bit roll-over to 0x00000100 */
	uint64_t val3 = spbm_energy_acc_update(&acc, 0x00000100);
	uint64_t expected_mj = (uint64_t)0xFFFFFF00 + 0x200;
	TEST_ASSERT(val3 > val2, "energy value must be strictly monotonic across 32-bit rollover");
	TEST_ASSERT(val3 == expected_mj * 1000, "val3 must equal (0xFFFFFF00 + 0x200) * 1000 uJ");

	/* Sample 4: multiple wrap-arounds */
	for (int i = 0; i < 5; i++) {
		uint64_t prev = val3;
		val3 = spbm_energy_acc_update(&acc, 0x00000100); /* delta = 0 */
		TEST_ASSERT(val3 == prev, "zero delta should not change accumulator");

		val3 = spbm_energy_acc_update(&acc, 0x80000000);
		TEST_ASSERT(val3 > prev, "positive delta must increase accumulator");
		prev = val3;

		val3 = spbm_energy_acc_update(&acc, 0x00000100); /* wrap-around */
		TEST_ASSERT(val3 > prev, "wrap-around must increase accumulator");
	}

	TEST_PASS();
}

/* --- Stage 4 Tests: Standard hwmon ABI Compliance --- */

static void test_hwmon_abi_compliance(void)
{
	/* Standard attributes must be allowed */
	TEST_ASSERT(spbm_hwmon_attr_is_standard("power1_input") == true, "power1_input is standard");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("power1_cap") == true, "power1_cap is standard");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("energy1_input") == true, "energy1_input is standard");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("temp1_input") == true, "temp1_input is standard");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("temp1_crit_alarm") == true, "temp1_crit_alarm is standard");

	/* Non-standard attributes must be rejected */
	TEST_ASSERT(spbm_hwmon_attr_is_standard("prochot") == false, "prochot must not be direct sysfs node");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("pl_level") == false, "pl_level must not be direct sysfs node");
	TEST_ASSERT(spbm_hwmon_attr_is_standard("tj_max_c") == false, "tj_max_c must not be direct sysfs node");

	TEST_PASS();
}

static void test_spbm_try_resolve_bounds(void)
{
	struct spbm_chan_def chans[] = {
		{ "SPBM_TEST_OFFSET", "test_chan" }
	};
	uint32_t offsets[1] = { 0xFFFFFFFF };
	uint64_t res_size = 0x1000;

	/* Valid offset */
	bool ok = spbm_try_resolve_bounds("SPBM_TEST_OFFSET", 0x100, res_size, chans, offsets, 1);
	TEST_ASSERT(ok == true, "valid offset should resolve");
	TEST_ASSERT(offsets[0] == 0x100, "offset must be set to 0x100");

	/* Out of bounds offset */
	offsets[0] = 0xFFFFFFFF;
	ok = spbm_try_resolve_bounds("SPBM_TEST_OFFSET", 0x1000, res_size, chans, offsets, 1);
	TEST_ASSERT(ok == false, "out of bounds offset should NOT resolve");
	TEST_ASSERT(offsets[0] == 0xFFFFFFFF, "offset must remain unchanged");

	TEST_PASS();
}

int main(void)
{
	printf("=== Running SPBM Unit Tests ===\n");
	test_dmi_match_valid_xfusion();
	test_dmi_match_valid_nvidia();
	test_dmi_reject_foreign_hardware();
	test_bounds_valid_range();
	test_bounds_out_of_range();
	test_mailbox_serialization();
	test_energy_monotonic_unwrapping();
	test_hwmon_abi_compliance();
	test_spbm_try_resolve_bounds();

	printf("\nSummary: %d tests run, %d failed.\n", tests_run, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
