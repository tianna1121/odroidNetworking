/*
 * Copyright (C) 2010 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __ARCH_CONFIG_H__
#define __ARCH_CONFIG_H__

#define USING_MEMORY 1
#define UMP_MEM_SIZE 256

#define ARCH_UMP_BACKEND_DEFAULT                USING_MEMORY
#define ARCH_UMP_MEMORY_ADDRESS_DEFAULT         0


#define ARCH_UMP_MEMORY_SIZE_DEFAULT            UMP_MEM_SIZE*1024*1024

#endif /* __ARCH_CONFIG_H__ */
