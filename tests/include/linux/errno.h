/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TEST_LINUX_ERRNO_H
#define _TEST_LINUX_ERRNO_H

#if defined(__has_include_next) && __has_include_next(<linux/errno.h>)
# include_next <linux/errno.h>
#else
# include <errno.h>
#endif

#endif
