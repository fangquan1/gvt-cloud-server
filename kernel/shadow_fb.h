/* SPDX-License-Identifier: MIT */
#ifndef _GVT_SHADOW_FB_H_
#define _GVT_SHADOW_FB_H_

#include <linux/mutex.h>
#include <linux/types.h>

struct intel_vgpu;
struct intel_vgpu_fb_info;
struct drm_i915_gem_object;

struct intel_vgpu_shadow_fb {
	struct mutex lock;
	bool enabled;
	bool allocated;
	bool dirty;
	bool blit_pending;
	struct drm_i915_gem_object *obj;
	u64 query_count;
	u64 primary_update_count;
	u64 blit_count;
	u64 blit_fail_count;
	u64 last_blit_ns;
	u64 page_cache_hits;
	u64 page_cache_misses;
	u64 gpu_blt_count;
	u64 gpu_blt_fail_count;
	u64 last_gpu_blt_ns;
	u64 source_gma;
	u64 source_gpa;
	u32 source_size;
	u32 shadow_size;
	u32 linear_stride;
	u32 width;
	u32 height;
	u32 stride;
	u32 source_x_offset;
	u32 source_y_offset;
	u32 drm_format;
	u64 drm_format_mod;
	void *page_cache;
	unsigned long *page_cache_gma;
	u32 page_cache_entries;
	struct drm_i915_gem_object *source_obj;
	struct intel_context *blt_ce;
	struct drm_i915_gem_object *blt_batch;
};

#if IS_ENABLED(CONFIG_DRM_I915_GVT)
void intel_vgpu_shadow_fb_init(struct intel_vgpu *vgpu);
void intel_vgpu_shadow_fb_cleanup(struct intel_vgpu *vgpu);
void intel_vgpu_shadow_fb_note_plane(struct intel_vgpu *vgpu,
				     const struct intel_vgpu_fb_info *info,
				     int plane_id);
bool intel_vgpu_shadow_fb_fill_info(struct intel_vgpu *vgpu,
					  struct intel_vgpu_fb_info *info);
struct drm_i915_gem_object *intel_vgpu_shadow_fb_get_object(struct intel_vgpu *vgpu);
int intel_vgpu_shadow_fb_update(struct intel_vgpu *vgpu);
void intel_vgpu_shadow_fb_debugfs_add(struct intel_vgpu *vgpu);
#else
static inline void intel_vgpu_shadow_fb_init(struct intel_vgpu *vgpu) {}
static inline void intel_vgpu_shadow_fb_cleanup(struct intel_vgpu *vgpu) {}
static inline void intel_vgpu_shadow_fb_note_plane(struct intel_vgpu *vgpu,
						  const struct intel_vgpu_fb_info *info,
						  int plane_id) {}
static inline bool intel_vgpu_shadow_fb_fill_info(struct intel_vgpu *vgpu,
					       struct intel_vgpu_fb_info *info) { return false; }
static inline struct drm_i915_gem_object *intel_vgpu_shadow_fb_get_object(struct intel_vgpu *vgpu) { return NULL; }
static inline int intel_vgpu_shadow_fb_update(struct intel_vgpu *vgpu) { return -ENODEV; }
static inline void intel_vgpu_shadow_fb_debugfs_add(struct intel_vgpu *vgpu) {}
#endif

#endif
