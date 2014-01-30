/*
 * Copyright (c) 2009-2011 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2009-2011 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rxe.h"
#include "rxe_loc.h"

#include <linux/kernel.h>

/*
 * lfsr (linear feedback shift register) with period 255
 */
static u8 rxe_get_key(void)
{
	static unsigned key = 1;

	key = key << 1;

	key |= (0 != (key & 0x100)) ^ (0 != (key & 0x10))
		^ (0 != (key & 0x80)) ^ (0 != (key & 0x40));

	key &= 0xff;

	return key;
}

int mem_check_range(struct rxe_mem *mem, u64 iova, size_t length)
{
	switch (mem->type) {
	case RXE_MEM_TYPE_DMA:
		return 0;

	case RXE_MEM_TYPE_MR:
	case RXE_MEM_TYPE_FMR:
		return ((iova < mem->iova) ||
			((iova + length) > (mem->iova + mem->length))) ?
			-EFAULT : 0;

	default:
		return -EFAULT;
	}
}

#define IB_ACCESS_REMOTE	(IB_ACCESS_REMOTE_READ		\
				| IB_ACCESS_REMOTE_WRITE	\
				| IB_ACCESS_REMOTE_ATOMIC)

static void rxe_mem_init(int access, struct rxe_mem *mem)
{
	u32 lkey = mem->pelem.index << 8 | rxe_get_key();
	u32 rkey = (access & IB_ACCESS_REMOTE) ? lkey : 0;

	if (mem->pelem.pool->type == RXE_TYPE_MR) {
		mem->ibmr.lkey		= lkey;
		mem->ibmr.rkey		= rkey;
	} else {
		mem->ibfmr.lkey		= lkey;
		mem->ibfmr.rkey		= rkey;
	}

	mem->pd			= NULL;
	mem->umem		= NULL;
	mem->lkey		= lkey;
	mem->rkey		= rkey;
	mem->state		= RXE_MEM_STATE_INVALID;
	mem->type		= RXE_MEM_TYPE_NONE;
	mem->va			= 0;
	mem->iova		= 0;
	mem->length		= 0;
	mem->offset		= 0;
	mem->access		= 0;
	mem->page_shift		= 0;
	mem->page_mask		= 0;
	mem->map_shift		= ilog2(RXE_BUF_PER_MAP);
	mem->map_mask		= 0;
	mem->num_buf		= 0;
	mem->max_buf		= 0;
	mem->num_map		= 0;
	mem->map		= NULL;
}

void rxe_mem_cleanup(void *arg)
{
	struct rxe_mem *mem = arg;
	int i;

	if (mem->umem)
		ib_umem_release(mem->umem);

	if (mem->map) {
		for (i = 0; i < mem->num_map; i++)
			kfree(mem->map[i]);

		kfree(mem->map);
	}
}

static int rxe_mem_alloc(struct rxe_dev *rxe, struct rxe_mem *mem, int num_buf)
{
	int i;
	int num_map;
	struct rxe_map **map = mem->map;

	num_map = (num_buf + RXE_BUF_PER_MAP - 1) / RXE_BUF_PER_MAP;

	mem->map = kmalloc(num_map*sizeof(map[0]), GFP_KERNEL);
	if (!mem->map)
		goto err1;

	for (i = 0; i < num_map; i++) {
		mem->map[i] = kmalloc(sizeof(*map[0]), GFP_KERNEL);
		if (!mem->map[i])
			goto err2;
	}

	BUG_ON(!is_power_of_2(RXE_BUF_PER_MAP));

	mem->map_shift	= ilog2(RXE_BUF_PER_MAP);
	mem->map_mask	= RXE_BUF_PER_MAP - 1;

	mem->num_buf = num_buf;
	mem->num_map = num_map;
	mem->max_buf = num_map*RXE_BUF_PER_MAP;

	return 0;

err2:
	for (i--; i >= 0; i--)
		kfree(mem->map[i]);

	kfree(mem->map);
err1:
	return -ENOMEM;
}

int rxe_mem_init_dma(struct rxe_dev *rxe, struct rxe_pd *pd,
	int access, struct rxe_mem *mem)
{
	rxe_mem_init(access, mem);

	mem->pd			= pd;
	mem->access		= access;
	mem->state		= RXE_MEM_STATE_VALID;
	mem->type		= RXE_MEM_TYPE_DMA;

	return 0;
}

int rxe_mem_init_phys(struct rxe_dev *rxe, struct rxe_pd *pd, int access,
		      u64 iova, struct ib_phys_buf *phys_buf, int num_buf,
		      struct rxe_mem *mem)
{
	int i;
	struct rxe_map **map;
	struct ib_phys_buf *buf;
	size_t length;
	int err;
	size_t min_size = (size_t)(-1L);
	size_t max_size = 0;
	int n;

	rxe_mem_init(access, mem);

	err = rxe_mem_alloc(rxe, mem, num_buf);
	if (err)
		goto err1;

	length			= 0;
	map			= mem->map;
	buf			= map[0]->buf;
	n			= 0;

	for (i = 0; i < num_buf; i++) {
		length	+= phys_buf->size;
		max_size = max_t(int, max_size, phys_buf->size);
		min_size = min_t(int, min_size, phys_buf->size);
		*buf++	= *phys_buf++;
		n++;

		if (n == RXE_BUF_PER_MAP) {
			map++;
			buf = map[0]->buf;
			n = 0;
		}
	}

	if (max_size == min_size && is_power_of_2(max_size)) {
		mem->page_shift		= ilog2(max_size);
		mem->page_mask		= max_size - 1;
	}

	mem->pd			= pd;
	mem->access		= access;
	mem->iova		= iova;
	mem->va			= iova;
	mem->length		= length;
	mem->state		= RXE_MEM_STATE_VALID;
	mem->type		= RXE_MEM_TYPE_MR;

	return 0;

err1:
	return err;
}

int rxe_mem_init_user(struct rxe_dev *rxe, struct rxe_pd *pd, u64 start,
	u64 length, u64 iova, int access, struct ib_udata *udata,
	struct rxe_mem *mem)
{
	int			i;
	struct rxe_map		**map;
	struct ib_phys_buf	*buf = NULL;
	struct ib_umem		*umem;
	struct ib_umem_chunk	*chunk;
	int			num_buf;
	void			*vaddr;
	int err;

	umem = ib_umem_get(pd->ibpd.uobject->context, start, length, access, 0);
	pr_warn("mem == %p\n", mem);
	pr_warn("umem == %p\n", umem);
	if (IS_ERR(umem)) {
		pr_warn("err %d from rxe_umem_get\n",
			 (int)PTR_ERR(umem));
		err = -EINVAL;
		goto err1;
	}

	mem->umem = umem;

	num_buf = 0;
	list_for_each_entry(chunk, &umem->chunk_list, list)
		num_buf += chunk->nents;

	rxe_mem_init(access, mem);

	pr_warn("num_buf == %d\n", num_buf);

	err = rxe_mem_alloc(rxe, mem, num_buf);
	if (err) {
		pr_warn("err %d from rxe_mem_alloc\n", err);
		goto err1;
	}

	BUG_ON(!is_power_of_2(umem->page_size));

	mem->page_shift		= ilog2(umem->page_size);
	mem->page_mask		= umem->page_size - 1;

	num_buf			= 0;
	map			= mem->map;
	pr_warn("length == %llu", (unsigned long long)length);
	pr_warn("map == %p", (void*)map);
	if (length > 0) {
		buf = map[0]->buf;
		pr_warn("buf == %p\n", buf);

		list_for_each_entry(chunk, &umem->chunk_list, list) {
			for (i = 0; i < chunk->nents; i++) {
			  pr_warn("chunk == %p\n", &chunk);
			  pr_warn("i == %d\n", i);
			  pr_warn("&chunk->page_list[i] == %p\n", &chunk->page_list[i]);
			  pr_warn("sg_page(&chunk->page_list[i]) == %p\n", sg_page(&chunk->page_list[i]));
				vaddr = sg_page(&chunk->page_list[i]);
				if (!vaddr) {
					pr_warn("null vaddr\n");
					dump_stack();
					err = -ENOMEM;
					goto err1;
				}

				buf->addr = (uintptr_t)vaddr;
				buf->size = umem->page_size;
				num_buf++;
				buf++;

				if (num_buf >= RXE_BUF_PER_MAP) {
					map++;
					buf = map[0]->buf;
					num_buf = 0;
				}
			}
		}
	}

	mem->pd			= pd;
	mem->umem		= umem;
	mem->access		= access;
	mem->length		= length;
	mem->iova		= iova;
	mem->va			= start;
	mem->offset		= umem->offset;
	mem->state		= RXE_MEM_STATE_VALID;
	mem->type		= RXE_MEM_TYPE_MR;

	return 0;

err1:
	return err;
}

int rxe_mem_init_fast(struct rxe_dev *rxe, struct rxe_pd *pd,
	int max_pages, struct rxe_mem *mem)
{
	int err;

	rxe_mem_init(0, mem);	/* TODO what access does this have */

	err = rxe_mem_alloc(rxe, mem, max_pages);
	if (err)
		goto err1;

	/* TODO what page size do we assume */

	mem->pd			= pd;
	mem->max_buf		= max_pages;
	mem->state		= RXE_MEM_STATE_FREE;
	mem->type		= RXE_MEM_TYPE_MR;

	return 0;

err1:
	return err;
}

int rxe_mem_init_mw(struct rxe_dev *rxe, struct rxe_pd *pd,
		    struct rxe_mem *mem)
{
	rxe_mem_init(0, mem);

	mem->pd			= pd;
	mem->state		= RXE_MEM_STATE_FREE;
	mem->type		= RXE_MEM_TYPE_MW;

	return 0;
}

int rxe_mem_init_fmr(struct rxe_dev *rxe, struct rxe_pd *pd, int access,
	struct ib_fmr_attr *attr, struct rxe_mem *mem)
{
	int err;

	if (attr->max_maps > rxe->attr.max_map_per_fmr) {
		pr_warn("max_mmaps = %d too big, max_map_per_fmr = %d\n",
			attr->max_maps, rxe->attr.max_map_per_fmr);
		err = -EINVAL;
		goto err1;
	}

	rxe_mem_init(access, mem);

	err = rxe_mem_alloc(rxe, mem, attr->max_pages);
	if (err)
		goto err1;

	mem->pd			= pd;
	mem->access		= access;
	mem->page_shift		 = attr->page_shift;
	mem->page_mask		= (1 << attr->page_shift) - 1;
	mem->max_buf		= attr->max_pages;
	mem->state		= RXE_MEM_STATE_FREE;
	mem->type		= RXE_MEM_TYPE_FMR;

	return 0;

err1:
	return err;
}

static void lookup_iova(
	struct rxe_mem	*mem,
	u64			iova,
	int			*m_out,
	int			*n_out,
	size_t			*offset_out)
{
	size_t			offset = iova - mem->iova + mem->offset;
	int			map_index;
	int			buf_index;
	u64			length;

	if (likely(mem->page_shift)) {
		*offset_out = offset & mem->page_mask;
		offset >>= mem->page_shift;
		*n_out = offset & mem->map_mask;
		*m_out = offset >> mem->map_shift;
	} else {
		map_index = 0;
		buf_index = 0;

		length = mem->map[map_index]->buf[buf_index].size;

		while (offset >= length) {
			offset -= length;
			buf_index++;

			if (buf_index == RXE_BUF_PER_MAP) {
				map_index++;
				buf_index = 0;
			}
			length = mem->map[map_index]->buf[buf_index].size;
		}

		*m_out = map_index;
		*n_out = buf_index;
		*offset_out = offset;
	}
}

void *iova_to_vaddr(struct rxe_mem *mem, u64 iova, int length)
{
	size_t offset;
	int m, n;
	void *addr;

	if (mem->state != RXE_MEM_STATE_VALID) {
		pr_warn("mem not in valid state\n");
		addr = NULL;
		goto out;
	}

	if (!mem->map) {
		addr = (void *)(uintptr_t)iova;
		goto out;
	}

	if (mem_check_range(mem, iova, length)) {
		pr_warn("range violation\n");
		addr = NULL;
		goto out;
	}

	lookup_iova(mem, iova, &m, &n, &offset);

	if (offset + length > mem->map[m]->buf[n].size) {
		pr_warn("crosses page boundary\n");
		addr = NULL;
		goto out;
	}

	addr = (void *)(uintptr_t)mem->map[m]->buf[n].addr + offset;

out:
	return addr;
}

/* copy data from a range (vaddr, vaddr+length-1) to or from
   a mem object starting at iova. Compute incremental value of
   crc32 if crcp is not zero. caller must hold a reference to mem */
int rxe_mem_copy(struct rxe_mem *mem, u64 iova, void *addr, int length,
		 enum copy_direction dir, u32 *crcp)
{
	int			err;
	int			bytes;
	u8			*va;
	struct rxe_map		**map;
	struct ib_phys_buf	*buf;
	int			m;
	int			i;
	size_t			offset;
	u32			crc = crcp ? (*crcp) : 0;

	if (mem->type == RXE_MEM_TYPE_DMA) {
		uint8_t *src, *dest;

		src  = (dir == direction_in) ?
			addr : ((void *)(uintptr_t)iova);

		dest = (dir == direction_in) ?
			((void *)(uintptr_t)iova) : addr;

		if (crcp)
			*crcp = crc32_le(*crcp, src, length);

		memcpy(dest, src, length);

		return 0;
	}

	BUG_ON(!mem->map);

	err = mem_check_range(mem, iova, length);
	if (err) {
		err = -EFAULT;
		goto err1;
	}

	lookup_iova(mem, iova, &m, &i, &offset);

	map	= mem->map + m;
	buf	= map[0]->buf + i;

	while (length > 0) {
		uint8_t *src, *dest;

		va	= (u8 *)(uintptr_t)buf->addr + offset;
		src  = (dir == direction_in) ? addr : va;
		dest = (dir == direction_in) ? va : addr;

		bytes	= buf->size - offset;

		if (bytes > length)
			bytes = length;

		if (crcp)
			crc = crc32_le(crc, src, bytes);

		memcpy(dest, src, bytes);

		length	-= bytes;
		addr	+= bytes;

		offset	= 0;
		buf++;
		i++;

		if (i == RXE_BUF_PER_MAP) {
			i = 0;
			map++;
			buf = map[0]->buf;
		}
	}

	if (crcp)
		*crcp = crc;

	return 0;

err1:
	return err;
}

/* copy data in or out of a wqe, i.e. sg list
   under the control of a dma descriptor */
int copy_data(
	struct rxe_dev		*rxe,
	struct rxe_pd		*pd,
	int			access,
	struct rxe_dma_info	*dma,
	void			*addr,
	int			length,
	enum copy_direction	dir,
	u32			*crcp)
{
	int			bytes;
	struct ib_sge		*sge	= &dma->sge[dma->cur_sge];
	int			offset	= dma->sge_offset;
	int			resid	= dma->resid;
	struct rxe_mem		*mem	= NULL;
	u64			iova;
	int			err;

	if (length == 0)
		return 0;

	if (length > resid) {
		err = -EINVAL;
		goto err2;
	}

	if (sge->length && (offset < sge->length)) {
		mem = lookup_mem(pd, access, sge->lkey, lookup_local);
		if (!mem) {
			err = -EINVAL;
			goto err1;
		}
	}

	while (length > 0) {
		bytes = length;

		if (offset >= sge->length) {
			if (mem) {
				rxe_drop_ref(mem);
				mem = NULL;
			}
			sge++;
			dma->cur_sge++;
			offset = 0;

			if (dma->cur_sge >= dma->num_sge) {
				err = -ENOSPC;
				goto err2;
			}

			if (sge->length) {
				mem = lookup_mem(pd, access, sge->lkey,
						 lookup_local);
				if (!mem) {
					err = -EINVAL;
					goto err1;
				}
			} else
				continue;
		}

		if (bytes > sge->length - offset)
			bytes = sge->length - offset;

		if (bytes > 0) {
			iova = sge->addr + offset;

			err = rxe_mem_copy(mem, iova, addr, bytes, dir, crcp);
			if (err)
				goto err2;

			offset	+= bytes;
			resid	-= bytes;
			length	-= bytes;
			addr	+= bytes;
		}
	}

	dma->sge_offset = offset;
	dma->resid	= resid;

	if (mem)
		rxe_drop_ref(mem);

	return 0;

err2:
	if (mem)
		rxe_drop_ref(mem);
err1:
	return err;
}

int advance_dma_data(struct rxe_dma_info *dma, unsigned int length)
{
	struct ib_sge		*sge	= &dma->sge[dma->cur_sge];
	int			offset	= dma->sge_offset;
	int			resid	= dma->resid;

	while (length) {
		unsigned int bytes;

		if (offset >= sge->length) {
			sge++;
			dma->cur_sge++;
			offset = 0;
			if (dma->cur_sge >= dma->num_sge)
				return -ENOSPC;
		}

		bytes = length;

		if (bytes > sge->length - offset)
			bytes = sge->length - offset;

		offset	+= bytes;
		resid	-= bytes;
		length	-= bytes;
	}

	dma->sge_offset = offset;
	dma->resid	= resid;

	return 0;
}

/* (1) find the mem (mr, fmr or mw) corresponding to lkey/rkey
       depending on lookup_type
   (2) verify that the (qp) pd matches the mem pd
   (3) verify that the mem can support the requested access
   (4) verify that mem state is valid */
struct rxe_mem *lookup_mem(struct rxe_pd *pd, int access, u32 key,
			   enum lookup_type type)
{
	struct rxe_mem *mem;
	struct rxe_dev *rxe = to_rdev(pd->ibpd.device);
	int index = key >> 8;

	if (index >= RXE_MIN_MR_INDEX && index <= RXE_MAX_MR_INDEX) {
		mem = rxe_pool_get_index(&rxe->mr_pool, index);
		if (!mem)
			goto err1;
	} else if (index >= RXE_MIN_FMR_INDEX && index <= RXE_MAX_FMR_INDEX) {
		mem = rxe_pool_get_index(&rxe->fmr_pool, index);
		if (!mem)
			goto err1;
	} else if (index >= RXE_MIN_MW_INDEX && index <= RXE_MAX_MW_INDEX) {
		mem = rxe_pool_get_index(&rxe->mw_pool, index);
		if (!mem)
			goto err1;
	} else
		goto err1;

	if ((type == lookup_local && mem->lkey != key)
		|| (type == lookup_remote && mem->rkey != key))
		goto err2;

	if (mem->pd != pd)
		goto err2;

	if (access && !(access & mem->access))
		goto err2;

	if (mem->state != RXE_MEM_STATE_VALID)
		goto err2;

	return mem;

err2:
	rxe_drop_ref(mem);
err1:
	return NULL;
}

int rxe_mem_map_pages(struct rxe_dev *rxe, struct rxe_mem *mem,
	u64 *page, int num_pages, u64 iova)
{
	int i;
	int num_buf;
	int err;
	struct rxe_map **map;
	struct ib_phys_buf *buf;
	int page_size;

	if (num_pages > mem->max_buf) {
		err = -EINVAL;
		goto err1;
	}

	num_buf		= 0;
	page_size	= 1 << mem->page_shift;
	map		= mem->map;
	buf		= map[0]->buf;

	for (i = 0; i < num_pages; i++) {
		buf->addr = *page++;
		buf->size = page_size;
		buf++;
		num_buf++;

		if (num_buf == RXE_BUF_PER_MAP) {
			map++;
			buf = map[0]->buf;
			num_buf = 0;
		}
	}

	mem->iova	= iova;
	mem->va		= iova;
	mem->length	= num_pages << mem->page_shift;
	mem->state	= RXE_MEM_STATE_VALID;

	return 0;

err1:
	return err;
}
