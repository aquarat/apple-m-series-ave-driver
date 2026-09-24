/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AVE_V4L2_H
#define AVE_V4L2_H

struct ave_device;

int ave_v4l2_register(struct ave_device *ave);
void ave_v4l2_unregister(struct ave_device *ave);

#endif
