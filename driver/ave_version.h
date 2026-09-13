/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Which host/firmware ABI to speak.
 *
 * AVE firmware has no interface-version check: a wrongly sized command makes
 * it spin rather than reply (docs/46). So the ABI cannot be probed and must be
 * chosen before the first command. The firmware iBoot loads comes from the OS
 * firmware bundle the Asahi installer pinned, which m1n1 reports as
 * /chosen/asahi,os-fw-version (docs/43).
 *
 * Both ABIs are kept. Asahi may rebase its stub firmware onto a newer macOS,
 * at which point the 26.6.2 analysis is the relevant one again.
 */
#ifndef __AVE_VERSION_H__
#define __AVE_VERSION_H__

struct ave_device;

enum ave_fw_abi {
	AVE_ABI_UNKNOWN = 0,
	AVE_ABI_MACOS_13_5,	/* AppleAVE2FW-6070.11.1, RTKit-2062.141.1 */
	AVE_ABI_MACOS_26_6,	/* AppleAVE2FW-9003.78.0, RTKit-3255.160.4 */
};

const char *ave_fw_abi_name(enum ave_fw_abi abi);
int ave_detect_fw_abi(struct ave_device *ave);

#endif /* __AVE_VERSION_H__ */
