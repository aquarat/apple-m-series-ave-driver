// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware ABI selection. See ave_version.h.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>

#include "ave.h"

static char *fw_abi;
module_param(fw_abi, charp, 0444);
MODULE_PARM_DESC(fw_abi, "override firmware ABI: \"13.5\" or \"26.6\" (default: from /chosen/asahi,os-fw-version)");

static const struct {
	const char	*version;	/* exact os-fw-version string */
	enum ave_fw_abi	abi;
} ave_known_versions[] = {
	{ "13.5",	AVE_ABI_MACOS_13_5 },
	{ "26.6",	AVE_ABI_MACOS_26_6 },
	{ "26.6.2",	AVE_ABI_MACOS_26_6 },
};

const char *ave_fw_abi_name(enum ave_fw_abi abi)
{
	switch (abi) {
	case AVE_ABI_MACOS_13_5:	return "macOS 13.5";
	case AVE_ABI_MACOS_26_6:	return "macOS 26.6";
	default:			return "unknown";
	}
}

static enum ave_fw_abi ave_lookup(const char *v)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ave_known_versions); i++)
		if (!strcmp(v, ave_known_versions[i].version))
			return ave_known_versions[i].abi;
	return AVE_ABI_UNKNOWN;
}

/*
 * Exact matches only. A version we have not analysed may differ in any
 * struct, and guessing the nearest one would send commands the firmware
 * silently hangs on - so refuse instead, and let the operator override.
 */
int ave_detect_fw_abi(struct ave_device *ave)
{
	const char *v = NULL;
	struct device_node *chosen;

	if (fw_abi) {
		v = fw_abi;
		dev_warn(ave->dev, "firmware ABI forced by module parameter: %s\n", v);
	} else {
		chosen = of_find_node_by_path("/chosen");
		if (chosen) {
			of_property_read_string(chosen, "asahi,os-fw-version", &v);
			of_node_put(chosen);
		}
		if (!v) {
			dev_err(ave->dev, "no /chosen/asahi,os-fw-version; set fw_abi=\n");
			return -ENODEV;
		}
	}

	ave->fw_abi = ave_lookup(v);
	if (ave->fw_abi == AVE_ABI_UNKNOWN) {
		dev_err(ave->dev, "OS firmware %s has no analysed AVE ABI; set fw_abi= to try one\n", v);
		return -EOPNOTSUPP;
	}
	dev_info(ave->dev, "OS firmware %s: using %s AVE ABI\n", v,
		 ave_fw_abi_name(ave->fw_abi));
	return 0;
}
