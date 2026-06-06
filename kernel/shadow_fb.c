// SPDX-License-Identifier: MIT
/*
 * Experimental GVT-g shadow framebuffer tracking.
 *
 * This is the first stage of the SPICE/VNC-friendly display path: it records
 * the guest primary plane that QEMU currently consumes as a tiled dmabuf. Later
 * stages will allocate a linear GEM object, BLT into it, and export that object
 * instead of exposing the guest's Y-tiled framebuffer directly.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_plane.h>
#include <linux/dma-buf.h>
#include <linux/debugfs.h>
#include <linux/file.h>
#include <linux/ioctl.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

#include "gem/i915_gem_dmabuf.h"
#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_object.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "i915_request.h"
#include "i915_vma.h"
#include "gvt.h"
#include "shadow_fb.h"

#define SHADOW_FB_PAGE_CACHE_ENTRIES 512
#define GEN8_DECODE_PTE(pte) ((pte) & GENMASK_ULL(63, 12))

#define GVT_SHADOWFB_IOC_MAGIC 'G'

struct gvt_shadowfb_info {
	__u32 version;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 fourcc;
	__u64 modifier;
	__u64 size;
	__u64 frame_id;
	__u64 last_blit_ns;
	__u64 last_frame_ns;
};

struct gvt_shadowfb_export {
	__u32 flags;
	__s32 fd;
};

struct gvt_shadowfb_blit {
	__u32 flags;
	__u32 timeout_ms;
	__u64 frame_id;
};

#define GVT_SHADOWFB_IOC_GET_INFO \
	_IOR(GVT_SHADOWFB_IOC_MAGIC, 0x01, struct gvt_shadowfb_info)

#define GVT_SHADOWFB_IOC_EXPORT_DMABUF \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x02, struct gvt_shadowfb_export)

#define GVT_SHADOWFB_IOC_TRIGGER_BLIT \
	_IOWR(GVT_SHADOWFB_IOC_MAGIC, 0x03, struct gvt_shadowfb_blit)

struct intel_vgpu_shadow_fb_source_info {
	struct intel_vgpu *vgpu;
	u64 start;
	u32 size;
};

static int intel_vgpu_shadow_src_get_pages(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct intel_vgpu_shadow_fb_source_info *info = obj->gvt_info;
	struct sg_table *st;
	struct scatterlist *sg;
	gen8_pte_t __iomem *gtt_entries;
	unsigned int page_num;
	int i, ret;

	if (!info || !info->vgpu)
		return -ENODEV;

	if (overflows_type(obj->base.size >> PAGE_SHIFT, page_num))
		return -E2BIG;

	page_num = obj->base.size >> PAGE_SHIFT;
	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	ret = sg_alloc_table(st, page_num, GFP_KERNEL);
	if (ret) {
		kfree(st);
		return ret;
	}

	gtt_entries = (gen8_pte_t __iomem *)to_gt(i915)->ggtt->gsm +
		(info->start >> PAGE_SHIFT);
	for_each_sg(st->sgl, sg, page_num, i) {
		dma_addr_t dma_addr = GEN8_DECODE_PTE(readq(&gtt_entries[i]));

		if (intel_gvt_dma_pin_guest_page(info->vgpu, dma_addr)) {
			ret = -EINVAL;
			goto out_unpin;
		}

		sg->offset = 0;
		sg->length = PAGE_SIZE;
		sg_dma_len(sg) = PAGE_SIZE;
		sg_dma_address(sg) = dma_addr;
	}

	__i915_gem_object_set_pages(obj, st);
	return 0;

out_unpin:
	for_each_sg(st->sgl, sg, i, ret)
		intel_gvt_dma_unmap_guest_page(info->vgpu, sg_dma_address(sg));
	sg_free_table(st);
	kfree(st);
	return ret;
}

static void intel_vgpu_shadow_src_put_pages(struct drm_i915_gem_object *obj,
						    struct sg_table *pages)
{
	struct intel_vgpu_shadow_fb_source_info *info = obj->gvt_info;
	struct scatterlist *sg;
	int i;

	if (info && info->vgpu) {
		for_each_sg(pages->sgl, sg, pages->nents, i)
			intel_gvt_dma_unmap_guest_page(info->vgpu,
						       sg_dma_address(sg));
	}

	sg_free_table(pages);
	kfree(pages);
}

static void intel_vgpu_shadow_src_release(struct drm_i915_gem_object *obj)
{
	kfree(obj->gvt_info);
	obj->gvt_info = NULL;
}

static const struct drm_i915_gem_object_ops intel_vgpu_shadow_src_ops = {
	.name = "i915_gem_object_gvt_shadow_src",
	.flags = I915_GEM_OBJECT_IS_PROXY,
	.get_pages = intel_vgpu_shadow_src_get_pages,
	.put_pages = intel_vgpu_shadow_src_put_pages,
	.release = intel_vgpu_shadow_src_release,
};

static struct drm_i915_gem_object *
intel_vgpu_shadow_fb_create_source_obj(struct intel_vgpu *vgpu,
					       const struct intel_vgpu_shadow_fb *shadow)
{
	static struct lock_class_key lock_class;
	struct drm_i915_private *i915 = vgpu->gvt->gt->i915;
	struct intel_vgpu_shadow_fb_source_info *info;
	struct drm_i915_gem_object *obj;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	obj = i915_gem_object_alloc();
	if (!obj) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}

	info->vgpu = vgpu;
	info->start = shadow->source_gma;
	info->size = shadow->source_size;
	drm_gem_private_object_init(&i915->drm, &obj->base,
				    roundup(shadow->source_size, PAGE_SIZE));
	i915_gem_object_init(obj, &intel_vgpu_shadow_src_ops, &lock_class, 0);
	i915_gem_object_set_readonly(obj);
	obj->gvt_info = info;
	obj->read_domains = I915_GEM_DOMAIN_GTT;
	obj->write_domain = 0;
	obj->tiling_and_stride = I915_TILING_Y | shadow->stride;

	return obj;
}

static struct intel_engine_cs *intel_vgpu_shadow_fb_copy_engine(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	int i;

	for (i = 0; i < ARRAY_SIZE(gt->engine_class[COPY_ENGINE_CLASS]); i++) {
		engine = gt->engine_class[COPY_ENGINE_CLASS][i];
		if (engine)
			return engine;
	}

	return NULL;
}

static int intel_vgpu_shadow_fb_ensure_blt_locked(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct intel_engine_cs *engine;

	if (!shadow->blt_ce) {
		engine = intel_vgpu_shadow_fb_copy_engine(vgpu->gvt->gt);
		if (!engine)
			return -ENODEV;

		shadow->blt_ce = intel_context_create(engine);
		if (IS_ERR(shadow->blt_ce)) {
			int ret = PTR_ERR(shadow->blt_ce);

			shadow->blt_ce = NULL;
			return ret;
		}
	}

	if (!shadow->blt_batch) {
		shadow->blt_batch = i915_gem_object_create_internal(vgpu->gvt->gt->i915,
								       PAGE_SIZE);
		if (IS_ERR(shadow->blt_batch)) {
			int ret = PTR_ERR(shadow->blt_batch);

			shadow->blt_batch = NULL;
			return ret;
		}
	}

	return 0;
}

static int intel_vgpu_shadow_fb_prepare_blt_batch(struct intel_vgpu *vgpu,
						  struct i915_vma *src,
						  struct i915_vma *dst,
						  struct i915_vma *batch)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct intel_gt *gt = vgpu->gvt->gt;
	u32 src_pitch = shadow->stride / 4;
	u32 dst_pitch = shadow->linear_stride;
	u32 *cs;

	cs = i915_gem_object_pin_map_unlocked(batch->obj, I915_MAP_WC);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(1);
	*cs++ = i915_mmio_reg_offset(BLIT_CCTL(shadow->blt_ce->engine->mmio_base));
	*cs++ = BLIT_CCTL_SRC_MOCS(gt->mocs.uc_index) |
		 BLIT_CCTL_DST_MOCS(gt->mocs.uc_index);

	*cs++ = GEN9_XY_FAST_COPY_BLT_CMD | (10 - 2) |
		XY_FAST_COPY_BLT_D0_SRC_TILE_MODE(YMAJOR);
	*cs++ = BLT_DEPTH_32 | dst_pitch;
	*cs++ = 0;
	*cs++ = shadow->height << 16 | shadow->width;
	*cs++ = lower_32_bits(i915_vma_offset(dst));
	*cs++ = upper_32_bits(i915_vma_offset(dst));
	*cs++ = 0;
	*cs++ = src_pitch;
	*cs++ = lower_32_bits(i915_vma_offset(src));
	*cs++ = upper_32_bits(i915_vma_offset(src));
	*cs++ = MI_BATCH_BUFFER_END;

	i915_gem_object_flush_map(batch->obj);
	i915_gem_object_unpin_map(batch->obj);
	return 0;
}

static int intel_vgpu_shadow_fb_gpu_blt_update_locked(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct i915_vma *src_vma = NULL, *dst_vma = NULL, *batch_vma = NULL;
	struct i915_request *rq;
	u64 start_ns;
	int ret;

	if (shadow->drm_format_mod != I915_FORMAT_MOD_Y_TILED ||
	    shadow->drm_format != DRM_FORMAT_XRGB8888 ||
	    GRAPHICS_VER(vgpu->gvt->gt->i915) < 9)
		return -EOPNOTSUPP;

	ret = intel_vgpu_shadow_fb_ensure_blt_locked(vgpu);
	if (ret)
		return ret;

	if (!shadow->source_obj) {
		shadow->source_obj = intel_vgpu_shadow_fb_create_source_obj(vgpu, shadow);
		if (IS_ERR(shadow->source_obj)) {
			ret = PTR_ERR(shadow->source_obj);
			shadow->source_obj = NULL;
			return ret;
		}
	}

	start_ns = ktime_get_ns();
	src_vma = i915_vma_instance(shadow->source_obj, shadow->blt_ce->vm, NULL);
	if (IS_ERR(src_vma))
		return PTR_ERR(src_vma);
	dst_vma = i915_vma_instance(shadow->obj, shadow->blt_ce->vm, NULL);
	if (IS_ERR(dst_vma))
		return PTR_ERR(dst_vma);
	batch_vma = i915_vma_instance(shadow->blt_batch, shadow->blt_ce->vm, NULL);
	if (IS_ERR(batch_vma))
		return PTR_ERR(batch_vma);

	ret = intel_context_pin(shadow->blt_ce);
	if (ret)
		return ret;

	ret = i915_vma_pin(src_vma, 0, 0, PIN_USER | PIN_HIGH);
	if (ret)
		goto out_unpin_ce;
	ret = i915_vma_pin(dst_vma, 0, 0, PIN_USER | PIN_HIGH);
	if (ret)
		goto out_unpin_src;
	ret = i915_vma_pin(batch_vma, 0, 0, PIN_USER | PIN_HIGH);
	if (ret)
		goto out_unpin_dst;

	ret = intel_vgpu_shadow_fb_prepare_blt_batch(vgpu, src_vma, dst_vma,
						       batch_vma);
	if (ret)
		goto out_unpin_batch;

	rq = intel_context_create_request(shadow->blt_ce);
	if (IS_ERR(rq)) {
		ret = PTR_ERR(rq);
		goto out_unpin_batch;
	}

	ret = i915_vma_move_to_active(batch_vma, rq, 0);
	if (!ret)
		ret = i915_vma_move_to_active(src_vma, rq, 0);
	if (!ret)
		ret = i915_vma_move_to_active(dst_vma, rq, EXEC_OBJECT_WRITE);
	if (!ret)
		ret = rq->engine->emit_bb_start(rq, i915_vma_offset(batch_vma),
						   i915_vma_size(batch_vma), 0);
	i915_request_get(rq);
	i915_request_add(rq);
	if (i915_request_wait(rq, 0, HZ / 2) < 0)
		ret = -ETIME;
	i915_request_put(rq);

out_unpin_batch:
	i915_vma_unpin(batch_vma);
out_unpin_dst:
	i915_vma_unpin(dst_vma);
out_unpin_src:
	i915_vma_unpin(src_vma);
out_unpin_ce:
	intel_context_unpin(shadow->blt_ce);

	shadow->last_gpu_blt_ns = ktime_get_ns() - start_ns;
	if (ret)
		shadow->gpu_blt_fail_count++;
	else
		shadow->gpu_blt_count++;

	return ret;
}


static int intel_vgpu_shadow_fb_ensure_page_cache_locked(struct intel_vgpu_shadow_fb *shadow)
{
	u32 i;

	if (shadow->page_cache && shadow->page_cache_gma)
		return 0;

	shadow->page_cache = kvzalloc(SHADOW_FB_PAGE_CACHE_ENTRIES * PAGE_SIZE,
					    GFP_KERNEL);
	shadow->page_cache_gma = kvcalloc(SHADOW_FB_PAGE_CACHE_ENTRIES,
					     sizeof(*shadow->page_cache_gma), GFP_KERNEL);
	if (!shadow->page_cache || !shadow->page_cache_gma) {
		kvfree(shadow->page_cache);
		kvfree(shadow->page_cache_gma);
		shadow->page_cache = NULL;
		shadow->page_cache_gma = NULL;
		shadow->page_cache_entries = 0;
		return -ENOMEM;
	}

	shadow->page_cache_entries = SHADOW_FB_PAGE_CACHE_ENTRIES;
	for (i = 0; i < shadow->page_cache_entries; i++)
		shadow->page_cache_gma[i] = ULONG_MAX;

	return 0;
}

static void intel_vgpu_shadow_fb_invalidate_page_cache_locked(struct intel_vgpu_shadow_fb *shadow)
{
	u32 i;

	for (i = 0; i < shadow->page_cache_entries; i++)
		shadow->page_cache_gma[i] = ULONG_MAX;
}

static u64 intel_vgpu_shadow_fb_swizzle_bit(unsigned int bit, u64 offset)
{
	return (offset & BIT_ULL(bit)) >> (bit - 6);
}

static u64 intel_vgpu_shadow_fb_apply_swizzle(u64 offset, unsigned int swizzle)
{
	switch (swizzle) {
	case I915_BIT_6_SWIZZLE_9:
		offset ^= intel_vgpu_shadow_fb_swizzle_bit(9, offset);
		break;
	case I915_BIT_6_SWIZZLE_9_10:
		offset ^= intel_vgpu_shadow_fb_swizzle_bit(9, offset) ^
			  intel_vgpu_shadow_fb_swizzle_bit(10, offset);
		break;
	case I915_BIT_6_SWIZZLE_9_11:
		offset ^= intel_vgpu_shadow_fb_swizzle_bit(9, offset) ^
			  intel_vgpu_shadow_fb_swizzle_bit(11, offset);
		break;
	case I915_BIT_6_SWIZZLE_9_10_11:
		offset ^= intel_vgpu_shadow_fb_swizzle_bit(9, offset) ^
			  intel_vgpu_shadow_fb_swizzle_bit(10, offset) ^
			  intel_vgpu_shadow_fb_swizzle_bit(11, offset);
		break;
	}

	return offset;
}

static u64 intel_vgpu_shadow_fb_x_tiled_offset(struct intel_vgpu *vgpu,
					       u32 x_bytes, u32 y, u32 stride)
{
	u64 x;
	u64 y_rem;
	u64 tile_y;
	u64 offset;

	tile_y = div64_u64_rem(y, 8, &y_rem);
	offset = tile_y * stride * 8;
	offset += y_rem * 512;
	offset += div64_u64_rem(x_bytes, 512, &x) << 12;
	offset += x;

	return intel_vgpu_shadow_fb_apply_swizzle(offset,
							   vgpu->gvt->gt->ggtt->bit_6_swizzle_x);
}

static u64 intel_vgpu_shadow_fb_y_tiled_offset(struct intel_vgpu *vgpu,
					       u32 x_bytes, u32 y, u32 stride)
{
	const unsigned int ytile_span = 16;
	const unsigned int ytile_height = 512;
	u64 x;
	u64 y_rem;
	u64 tile_y;
	u64 offset;

	/* Keep this formula aligned with i915_gem_client_blt.c:tiled_offset(). */
	tile_y = div64_u64_rem(y, 32, &y_rem);
	offset = tile_y * stride * 32;
	offset += y_rem * ytile_span;
	offset += div64_u64_rem(x_bytes, ytile_span, &x) * ytile_height;
	offset += x;

	return intel_vgpu_shadow_fb_apply_swizzle(offset,
							   vgpu->gvt->gt->ggtt->bit_6_swizzle_y);
}

static u32 intel_vgpu_shadow_fb_contiguous_bytes(const struct intel_vgpu_shadow_fb *shadow,
						 u32 x_bytes, u32 limit)
{
	switch (shadow->drm_format_mod) {
	case DRM_FORMAT_MOD_LINEAR:
		return limit;
	case I915_FORMAT_MOD_X_TILED:
		return min(limit, 512 - (x_bytes % 512));
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Yf_TILED:
		return min(limit, 16 - (x_bytes % 16));
	default:
		return limit;
	}
}

static u64 intel_vgpu_shadow_fb_source_offset(struct intel_vgpu *vgpu,
					      const struct intel_vgpu_shadow_fb *shadow,
					      u32 x_bytes, u32 y)
{
	switch (shadow->drm_format_mod) {
	case DRM_FORMAT_MOD_LINEAR:
		return (u64)y * shadow->stride + x_bytes;
	case I915_FORMAT_MOD_X_TILED:
		return intel_vgpu_shadow_fb_x_tiled_offset(vgpu, x_bytes, y,
							      shadow->stride);
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Yf_TILED:
		return intel_vgpu_shadow_fb_y_tiled_offset(vgpu, x_bytes, y,
							      shadow->stride);
	default:
		return (u64)y * shadow->stride + x_bytes;
	}
}

static u32 intel_vgpu_shadow_fb_cpp(u32 drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return 4;
	default:
		return 4;
	}
}

static u32 intel_vgpu_shadow_fb_linear_stride(const struct intel_vgpu_fb_info *info)
{
	switch (info->drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return info->width * 4;
	default:
		return info->stride;
	}
}

static int intel_vgpu_shadow_fb_alloc_locked(struct intel_vgpu *vgpu,
					    const struct intel_vgpu_fb_info *info)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct drm_i915_private *i915 = vgpu->gvt->gt->i915;
	struct drm_i915_gem_object *obj;
	int ret;
	u32 stride = intel_vgpu_shadow_fb_linear_stride(info);
	u32 size = PAGE_ALIGN(stride * info->height);

	if (shadow->obj && shadow->shadow_size == size &&
	    shadow->linear_stride == stride) {
		shadow->allocated = true;
		return 0;
	}

	obj = i915_gem_object_create_shmem(i915, size);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);

		shadow->allocated = false;
		pr_err("gvt: vgpu%d: shadow_fb allocation failed size=%u ret=%d\n",
		       vgpu->id, size, ret);
		return ret;
	}

	if (shadow->obj)
		i915_gem_object_put(shadow->obj);

	shadow->obj = obj;
	shadow->shadow_size = size;
	shadow->linear_stride = stride;
	shadow->allocated = true;
	pr_info("gvt: vgpu%d: shadow_fb allocated %u bytes linear_stride=%u\n",
		vgpu->id, size, stride);

	return 0;
}

void intel_vgpu_shadow_fb_init(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;

	memset(shadow, 0, sizeof(*shadow));
	mutex_init(&shadow->lock);
	/*
	 * The native gvt-stream display backend consumes VFIO display dmabuf
	 * directly.  Keeping the old shadow framebuffer path enabled adds a
	 * full-frame BLT on every primary update and can destabilize Windows
	 * GVT-g video playback, so keep it off for this route even if the
	 * boot-time i915.enable_gvt_shadowfb parameter is still present.
	 */
	shadow->enabled = false;
}

void intel_vgpu_shadow_fb_cleanup(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;

	if (!shadow->enabled)
		return;

	mutex_lock(&shadow->lock);
	if (shadow->obj) {
		i915_gem_object_put(shadow->obj);
		shadow->obj = NULL;
	}
	kvfree(shadow->page_cache);
	kvfree(shadow->page_cache_gma);
	shadow->page_cache = NULL;
	shadow->page_cache_gma = NULL;
	shadow->page_cache_entries = 0;
	if (shadow->source_obj) {
		i915_gem_object_put(shadow->source_obj);
		shadow->source_obj = NULL;
	}
	if (shadow->blt_batch) {
		i915_gem_object_put(shadow->blt_batch);
		shadow->blt_batch = NULL;
	}
	if (shadow->blt_ce) {
		intel_context_put(shadow->blt_ce);
		shadow->blt_ce = NULL;
	}
	shadow->allocated = false;
	shadow->dirty = false;
	shadow->blit_pending = false;
	shadow->shadow_size = 0;
	shadow->linear_stride = 0;
	mutex_unlock(&shadow->lock);

	pr_info("gvt: vgpu%d: shadow_fb cleaned\n", vgpu->id);
}

void intel_vgpu_shadow_fb_note_plane(struct intel_vgpu *vgpu,
				     const struct intel_vgpu_fb_info *info,
				     int plane_id)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	bool changed;

	if (!shadow->enabled || plane_id != DRM_PLANE_TYPE_PRIMARY)
		return;

	mutex_lock(&shadow->lock);
	shadow->query_count++;
	changed = shadow->width != info->width ||
		  shadow->height != info->height ||
		  shadow->stride != info->stride ||
		  shadow->drm_format != info->drm_format ||
		  shadow->drm_format_mod != info->drm_format_mod ||
		  shadow->source_x_offset != info->x_pos ||
		  shadow->source_y_offset != info->y_pos ||
		  shadow->source_gma != info->start ||
		  shadow->source_gpa != info->start_gpa ||
		  shadow->source_size != info->size;

	if (changed) {
		if (shadow->source_obj) {
			i915_gem_object_put(shadow->source_obj);
			shadow->source_obj = NULL;
		}
		shadow->width = info->width;
		shadow->height = info->height;
		shadow->stride = info->stride;
		shadow->source_x_offset = info->x_pos;
		shadow->source_y_offset = info->y_pos;
		shadow->drm_format = info->drm_format;
		shadow->drm_format_mod = info->drm_format_mod;
		shadow->source_gma = info->start;
		shadow->source_gpa = info->start_gpa;
		shadow->source_size = info->size;
		shadow->dirty = true;
		shadow->blit_pending = false;
		shadow->primary_update_count++;
		intel_vgpu_shadow_fb_alloc_locked(vgpu, info);
		drm_dbg(&vgpu->gvt->gt->i915->drm,
			"gvt: vgpu%d: shadow_fb primary %ux%u stride=%u offset=%u,%u format=0x%x tiling=0x%llx size=%u\n",
			vgpu->id, shadow->width, shadow->height,
			shadow->stride, shadow->source_x_offset,
			shadow->source_y_offset, shadow->drm_format,
			shadow->drm_format_mod, shadow->source_size);
	}
	mutex_unlock(&shadow->lock);
}

bool intel_vgpu_shadow_fb_fill_info(struct intel_vgpu *vgpu,
				       struct intel_vgpu_fb_info *info)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	bool ret = false;

	mutex_lock(&shadow->lock);
	if (shadow->enabled && shadow->allocated && shadow->obj &&
	    shadow->shadow_size && shadow->linear_stride) {
		info->start = 0;
		info->start_gpa = 0;
		info->drm_format_mod = DRM_FORMAT_MOD_LINEAR;
		info->drm_format = shadow->drm_format;
		info->width = shadow->width;
		info->height = shadow->height;
		info->stride = shadow->linear_stride;
		info->x_pos = 0;
		info->y_pos = 0;
		info->size = shadow->shadow_size;
		ret = true;
	}
	mutex_unlock(&shadow->lock);

	return ret;
}

struct drm_i915_gem_object *intel_vgpu_shadow_fb_get_object(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct drm_i915_gem_object *obj = NULL;

	mutex_lock(&shadow->lock);
	if (shadow->enabled && shadow->allocated && shadow->obj)
		obj = i915_gem_object_get(shadow->obj);
	mutex_unlock(&shadow->lock);

	return obj;
}

int intel_vgpu_shadow_fb_update(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct drm_i915_gem_object *obj;
	void *dst;
	u32 width_bytes;
	u32 cpp;
	u64 frame_cache_hits = 0;
	u64 frame_cache_misses = 0;
	u64 start_ns;
	int ret = 0;
	u32 y;

	mutex_lock(&shadow->lock);
	if (!shadow->enabled || !shadow->allocated || !shadow->obj ||
	    !shadow->source_gpa || !shadow->width || !shadow->height) {
		ret = -ENODEV;
		goto out_unlock;
	}

	obj = shadow->obj;
	width_bytes = min(shadow->linear_stride, shadow->stride);
	cpp = intel_vgpu_shadow_fb_cpp(shadow->drm_format);
	ret = intel_vgpu_shadow_fb_ensure_page_cache_locked(shadow);
	if (ret)
		goto out_unlock;
	intel_vgpu_shadow_fb_invalidate_page_cache_locked(shadow);
	shadow->blit_pending = true;
	start_ns = ktime_get_ns();

	ret = intel_vgpu_shadow_fb_gpu_blt_update_locked(vgpu);
	if (!ret) {
		shadow->dirty = false;
		goto out_finish;
	}

	ret = i915_gem_object_lock(obj, NULL);
	if (ret)
		goto out_finish;

	dst = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(dst)) {
		ret = PTR_ERR(dst);
		i915_gem_object_unlock(obj);
		goto out_finish;
	}

	for (y = 0; y < shadow->height; y++) {
		u32 copied = 0;

		while (copied < width_bytes) {
			u32 chunk = width_bytes - copied;
			u32 src_x = copied + shadow->source_x_offset * cpp;
			u32 src_y = y + shadow->source_y_offset;
			u32 cache_index;
			u32 page_off;
			u32 page_rem;
			u64 src_off;
			u64 src_gma;
			u64 src_gma_page;
			unsigned long src_gpa;
			void *cache_page;

			chunk = intel_vgpu_shadow_fb_contiguous_bytes(shadow,
								       src_x, chunk);
			src_off = intel_vgpu_shadow_fb_source_offset(vgpu, shadow,
								     src_x, src_y);
			src_gma = shadow->source_gma + src_off;
			src_gma_page = src_gma & PAGE_MASK;
			page_off = offset_in_page(src_gma);
			page_rem = PAGE_SIZE - page_off;
			chunk = min(chunk, page_rem);
			cache_index = (src_gma_page >> PAGE_SHIFT) &
				      (shadow->page_cache_entries - 1);
			cache_page = shadow->page_cache + cache_index * PAGE_SIZE;

			if (shadow->page_cache_gma[cache_index] != src_gma_page) {
				src_gpa = intel_vgpu_gma_to_gpa(vgpu->gtt.ggtt_mm,
								      src_gma_page);
				if (src_gpa == INTEL_GVT_INVALID_ADDR) {
					ret = -EFAULT;
					goto out_unpin;
				}
				ret = intel_gvt_read_gpa(vgpu, src_gpa, cache_page,
							 PAGE_SIZE);
				if (ret)
					goto out_unpin;
				shadow->page_cache_gma[cache_index] = src_gma_page;
				frame_cache_misses++;
			} else {
				frame_cache_hits++;
			}

			memcpy(dst + (u64)y * shadow->linear_stride + copied,
			       cache_page + page_off, chunk);

			copied += chunk;
		}
	}

	i915_gem_object_flush_map(obj);
	shadow->dirty = false;

out_unpin:
	i915_gem_object_unpin_map(obj);
	i915_gem_object_unlock(obj);
out_finish:
	shadow->blit_pending = false;
	shadow->last_blit_ns = ktime_get_ns() - start_ns;
	shadow->page_cache_hits = frame_cache_hits;
	shadow->page_cache_misses = frame_cache_misses;
	if (ret)
		shadow->blit_fail_count++;
	else
		shadow->blit_count++;
out_unlock:
	mutex_unlock(&shadow->lock);

	if (!ret)
		gvt_dbg_dpy("vgpu%d: shadow_fb update successful (%llu ns)\n",
			    vgpu->id, shadow->last_blit_ns);

	return ret;
}

static int intel_vgpu_shadow_fb_show(struct seq_file *m, void *data)
{
	struct intel_vgpu *vgpu = m->private;
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;

	mutex_lock(&shadow->lock);
	seq_printf(m, "enabled:\t%s\n", shadow->enabled ? "yes" : "no");
	seq_printf(m, "allocated:\t%s\n", shadow->allocated ? "yes" : "no");
	seq_printf(m, "dirty:\t\t%s\n", shadow->dirty ? "yes" : "no");
	seq_printf(m, "query_count:\t%llu\n", shadow->query_count);
	seq_printf(m, "primary_updates:\t%llu\n",
		   shadow->primary_update_count);
	seq_printf(m, "blit_count:\t%llu\n", shadow->blit_count);
	seq_printf(m, "blit_fail_count:\t%llu\n", shadow->blit_fail_count);
	seq_printf(m, "gpu_blt_count:\t%llu\n", shadow->gpu_blt_count);
	seq_printf(m, "gpu_blt_fail_count:\t%llu\n", shadow->gpu_blt_fail_count);
	seq_printf(m, "last_gpu_blt_ns:\t%llu\n", shadow->last_gpu_blt_ns);
	seq_printf(m, "blit_pending:\t%s\n", shadow->blit_pending ? "yes" : "no");
	seq_printf(m, "last_blit_ns:\t%llu\n", shadow->last_blit_ns);
	seq_printf(m, "page_cache_hits:\t%llu\n", shadow->page_cache_hits);
	seq_printf(m, "page_cache_misses:\t%llu\n", shadow->page_cache_misses);
	seq_printf(m, "page_cache_entries:\t%u\n", shadow->page_cache_entries);
	seq_printf(m, "source_gma:\t0x%llx\n", shadow->source_gma);
	seq_printf(m, "source_gpa:\t0x%llx\n", shadow->source_gpa);
	seq_printf(m, "source_size:\t%u\n", shadow->source_size);
	seq_printf(m, "shadow_size:\t%u\n", shadow->shadow_size);
	seq_printf(m, "linear_stride:\t%u\n", shadow->linear_stride);
	seq_puts(m, "\nSource framebuffer info:\n");
	seq_printf(m, "  width:\t%u\n", shadow->width);
	seq_printf(m, "  height:\t%u\n", shadow->height);
	seq_printf(m, "  stride:\t%u\n", shadow->stride);
	seq_printf(m, "  offset:\t%u,%u\n", shadow->source_x_offset,
		   shadow->source_y_offset);
	seq_printf(m, "  format:\t0x%x\n", shadow->drm_format);
	seq_printf(m, "  tiling:\t0x%llx\n", shadow->drm_format_mod);
	mutex_unlock(&shadow->lock);

	return 0;
}

static int intel_vgpu_shadow_fb_open(struct inode *inode, struct file *file)
{
	return single_open(file, intel_vgpu_shadow_fb_show, inode->i_private);
}


static int intel_vgpu_shadow_fb_copy_info_to_user(struct intel_vgpu *vgpu,
						  void __user *uarg)
{
	struct intel_vgpu_shadow_fb *shadow = &vgpu->shadow_fb;
	struct gvt_shadowfb_info info = { .version = 1 };

	mutex_lock(&shadow->lock);
	if (!shadow->enabled || !shadow->allocated || !shadow->obj) {
		mutex_unlock(&shadow->lock);
		return -ENODEV;
	}

	info.width = shadow->width;
	info.height = shadow->height;
	info.stride = shadow->linear_stride;
	info.fourcc = shadow->drm_format;
	info.modifier = DRM_FORMAT_MOD_LINEAR;
	info.size = shadow->shadow_size;
	info.frame_id = shadow->blit_count;
	info.last_blit_ns = shadow->last_blit_ns;
	mutex_unlock(&shadow->lock);

	if (copy_to_user(uarg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int intel_vgpu_shadow_fb_export_fd(struct intel_vgpu *vgpu,
					  void __user *uarg)
{
	struct gvt_shadowfb_export export = { .fd = -1 };
	struct drm_i915_gem_object *obj;
	struct dma_buf *dmabuf;
	int fd;
	int ret;

	if (uarg && copy_from_user(&export, uarg, sizeof(export)))
		return -EFAULT;

	ret = intel_vgpu_shadow_fb_update(vgpu);
	if (ret)
		return ret;

	obj = intel_vgpu_shadow_fb_get_object(vgpu);
	if (!obj)
		return -ENODEV;

	dmabuf = i915_gem_prime_export(&obj->base, DRM_CLOEXEC | DRM_RDWR);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto out_put_obj;
	}

	fd = get_unused_fd_flags(DRM_CLOEXEC | DRM_RDWR);
	if (fd < 0) {
		ret = fd;
		goto out_put_dmabuf;
	}

	export.fd = fd;
	if (uarg && copy_to_user(uarg, &export, sizeof(export))) {
		put_unused_fd(fd);
		ret = -EFAULT;
		goto out_put_dmabuf;
	}

	fd_install(fd, dmabuf->file);
	i915_gem_object_put(obj);
	return fd;

out_put_dmabuf:
	dma_buf_put(dmabuf);
out_put_obj:
	i915_gem_object_put(obj);
	return ret;
}

static int intel_vgpu_shadow_fb_trigger_blit(struct intel_vgpu *vgpu,
					     void __user *uarg)
{
	struct gvt_shadowfb_blit blit = {};
	int ret;

	if (uarg && copy_from_user(&blit, uarg, sizeof(blit)))
		return -EFAULT;

	ret = intel_vgpu_shadow_fb_update(vgpu);
	if (ret)
		return ret;

	blit.frame_id = vgpu->shadow_fb.blit_count;
	if (uarg && copy_to_user(uarg, &blit, sizeof(blit)))
		return -EFAULT;

	return 0;
}

static long intel_vgpu_shadow_fb_ioctl(struct file *file, unsigned int cmd,
				       unsigned long arg)
{
	struct seq_file *seq = file->private_data;
	struct intel_vgpu *vgpu = seq ? seq->private : NULL;
	void __user *uarg = (void __user *)arg;

	if (!vgpu)
		return -ENODEV;

	switch (cmd) {
	case GVT_SHADOWFB_IOC_GET_INFO:
		return intel_vgpu_shadow_fb_copy_info_to_user(vgpu, uarg);
	case GVT_SHADOWFB_IOC_EXPORT_DMABUF:
		return intel_vgpu_shadow_fb_export_fd(vgpu, uarg);
	case GVT_SHADOWFB_IOC_TRIGGER_BLIT:
		return intel_vgpu_shadow_fb_trigger_blit(vgpu, uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations intel_vgpu_shadow_fb_fops = {
	.owner = THIS_MODULE,
	.open = intel_vgpu_shadow_fb_open,
	.read = seq_read,
	.unlocked_ioctl = intel_vgpu_shadow_fb_ioctl,
	.compat_ioctl = intel_vgpu_shadow_fb_ioctl,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t intel_vgpu_shadow_fb_blit_write(struct file *file,
					       const char __user *ubuf,
					       size_t len, loff_t *ppos)
{
	struct intel_vgpu *vgpu = file->private_data;

	if (!len)
		return 0;

	intel_vgpu_shadow_fb_update(vgpu);
	return len;
}

static int intel_vgpu_shadow_fb_blit_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations intel_vgpu_shadow_fb_blit_fops = {
	.owner = THIS_MODULE,
	.open = intel_vgpu_shadow_fb_blit_open,
	.write = intel_vgpu_shadow_fb_blit_write,
	.llseek = no_llseek,
};

void intel_vgpu_shadow_fb_debugfs_add(struct intel_vgpu *vgpu)
{
	if (!vgpu->debugfs)
		return;

	debugfs_create_file("shadow_fb", 0444, vgpu->debugfs, vgpu,
			    &intel_vgpu_shadow_fb_fops);
	debugfs_create_file("shadow_fb_blit", 0200, vgpu->debugfs, vgpu,
			    &intel_vgpu_shadow_fb_blit_fops);
}
