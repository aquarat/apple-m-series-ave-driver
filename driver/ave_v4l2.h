/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AVE_V4L2_H
#define AVE_V4L2_H

#include <linux/types.h>

struct ave_device;

int ave_v4l2_register(struct ave_device *ave);
void ave_v4l2_unregister(struct ave_device *ave);

/* System sleep (docs/86). All are no-ops when V4L2 is not registered. */
void ave_v4l2_pm_quiesce(struct ave_device *ave);	/* returns with hw lock held */
void ave_v4l2_pm_lock(struct ave_device *ave);
void ave_v4l2_pm_unlock(struct ave_device *ave);
void ave_v4l2_pm_resume(struct ave_device *ave);
bool ave_v4l2_idle(struct ave_device *ave);

#endif
