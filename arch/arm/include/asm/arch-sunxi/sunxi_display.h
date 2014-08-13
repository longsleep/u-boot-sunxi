/*
 * (C) Copyright 2014 Luc Verhaegen <libv@skynet.be>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation's version 2 and any
 * later version the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _SUNXI_DISPLAY_H_
#define _SUNXI_DISPLAY_H_

#ifdef CONFIG_VIDEO_DT_SIMPLEFB
void sunxi_simplefb_setup(void *blob);
#endif

#endif /* _SUNXI_DISPLAY_H_ */
