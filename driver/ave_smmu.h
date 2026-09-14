/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple AVE - read-only watch on the dart-ave0 SMMU's fault status (docs/56).
 */
#ifndef __AVE_SMMU_H__
#define __AVE_SMMU_H__

struct ave_device;

/*
 * Map the SMMU, log a baseline, and add a shared read-only handler on its
 * interrupt. No-op (returns 0) unless smmu_watch=1. Requires ave->powered.
 */
int ave_smmu_init(struct ave_device *ave);

/*
 * Stop the handler touching the block and wait out any running instance.
 * Must run before the power reference is dropped: a read of a gated block
 * hangs the fabric (docs/24). Idempotent.
 */
void ave_smmu_quiesce(struct ave_device *ave);

#endif /* __AVE_SMMU_H__ */
