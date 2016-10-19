/*
 *  gstvaapivideomemory.c - Gstreamer/VA video memory
 *
 *  Copyright (C) 2013 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#include "gstcompat.h"
#include <unistd.h>
#include <gst/vaapi/gstvaapisurface_drm.h>
#include <gst/vaapi/gstvaapisurfacepool.h>
#include <gst/vaapi/gstvaapiimagepool.h>
#include "gstvaapivideomemory.h"
#include "gstvaapipluginutil.h"

GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);
GST_DEBUG_CATEGORY_STATIC (gst_debug_vaapivideomemory);
#define GST_CAT_DEFAULT gst_debug_vaapivideomemory

#ifndef GST_VIDEO_INFO_FORMAT_STRING
#define GST_VIDEO_INFO_FORMAT_STRING(vip) \
  gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (vip))
#endif

/* ------------------------------------------------------------------------ */
/* --- GstVaapiVideoMemory                                              --- */
/* ------------------------------------------------------------------------ */

static void gst_vaapi_video_memory_reset_image (GstVaapiVideoMemory * mem);

static void
_init_performance_debug (void)
{
#ifndef GST_DISABLE_GST_DEBUG
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_GET (CAT_PERFORMANCE, "GST_PERFORMANCE");
    g_once_init_leave (&_init, 1);
  }
#endif
}

static void
_init_vaapi_video_memory_debug (void)
{
#ifndef GST_DISABLE_GST_DEBUG
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_debug_vaapivideomemory, "vaapivideomemory", 0,
        "VA-API video memory allocator");
    g_once_init_leave (&_init, 1);
  }
#endif
}

static inline void
reset_image_usage (GstVaapiImageUsageFlags * flag)
{
  _init_performance_debug ();
  GST_CAT_INFO (CAT_PERFORMANCE, "derive image failed, fallbacking to copy");
  *flag = GST_VAAPI_IMAGE_USAGE_FLAG_NATIVE_FORMATS;
}

static inline gboolean
use_native_formats (GstVaapiImageUsageFlags flag)
{
  return flag == GST_VAAPI_IMAGE_USAGE_FLAG_NATIVE_FORMATS;
}

static inline gboolean
use_direct_rendering (GstVaapiImageUsageFlags flag)
{
  return flag == GST_VAAPI_IMAGE_USAGE_FLAG_DIRECT_RENDER;
}

static inline gboolean
use_direct_uploading (GstVaapiImageUsageFlags flag)
{
  return flag == GST_VAAPI_IMAGE_USAGE_FLAG_DIRECT_UPLOAD;
}

static guchar *
get_image_data (GstVaapiImage * image)
{
  guchar *data;
  VAImage va_image;

  data = gst_vaapi_image_get_plane (image, 0);
  if (!data || !gst_vaapi_image_get_image (image, &va_image))
    return NULL;

  data -= va_image.offsets[0];
  return data;
}

static GstVaapiImage *
new_image (GstVaapiDisplay * display, const GstVideoInfo * vip)
{
  if (!GST_VIDEO_INFO_WIDTH (vip) || !GST_VIDEO_INFO_HEIGHT (vip))
    return NULL;
  return gst_vaapi_image_new (display, GST_VIDEO_INFO_FORMAT (vip),
      GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip));
}

static gboolean
ensure_image (GstVaapiVideoMemory * mem)
{
  if (!mem->image && !use_native_formats (mem->usage_flag)) {
    mem->image = gst_vaapi_surface_derive_image (mem->surface);
    if (!mem->image) {
      reset_image_usage (&mem->usage_flag);
    } else if (gst_vaapi_surface_get_format (mem->surface) !=
        GST_VIDEO_INFO_FORMAT (mem->image_info)) {
      gst_vaapi_object_replace (&mem->image, NULL);
      reset_image_usage (&mem->usage_flag);
    }
  }

  if (!mem->image) {
    GstVaapiVideoAllocator *const allocator =
        GST_VAAPI_VIDEO_ALLOCATOR_CAST (GST_MEMORY_CAST (mem)->allocator);

    mem->image = gst_vaapi_video_pool_get_object (allocator->image_pool);
    if (!mem->image)
      return FALSE;
  }
  gst_vaapi_video_meta_set_image (mem->meta, mem->image);
  return TRUE;
}

static gboolean
ensure_image_is_current (GstVaapiVideoMemory * mem)
{
  if (!use_native_formats (mem->usage_flag))
    return TRUE;

  if (!GST_VAAPI_VIDEO_MEMORY_FLAG_IS_SET (mem,
          GST_VAAPI_VIDEO_MEMORY_FLAG_IMAGE_IS_CURRENT)) {
    if (!gst_vaapi_surface_get_image (mem->surface, mem->image))
      return FALSE;

    GST_VAAPI_VIDEO_MEMORY_FLAG_SET (mem,
        GST_VAAPI_VIDEO_MEMORY_FLAG_IMAGE_IS_CURRENT);
  }
  return TRUE;
}

static GstVaapiSurface *
new_surface (GstVaapiDisplay * display, const GstVideoInfo * vip,
    GstVaapiImageUsageFlags usage_flag)
{
  GstVaapiSurface *surface;
  GstVaapiChromaType chroma_type;

  /* Try with explicit format first */
  if (!use_native_formats (usage_flag) &&
      GST_VIDEO_INFO_FORMAT (vip) != GST_VIDEO_FORMAT_ENCODED) {
    surface = gst_vaapi_surface_new_with_format (display,
        GST_VIDEO_INFO_FORMAT (vip), GST_VIDEO_INFO_WIDTH (vip),
        GST_VIDEO_INFO_HEIGHT (vip));
    if (surface)
      return surface;
  }

  /* Try to pick something compatible, i.e. with same chroma type */
  chroma_type =
      gst_vaapi_video_format_get_chroma_type (GST_VIDEO_INFO_FORMAT (vip));
  if (!chroma_type)
    return NULL;
  return gst_vaapi_surface_new (display, chroma_type,
      GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip));
}

static GstVaapiSurfaceProxy *
new_surface_proxy (GstVaapiVideoMemory * mem)
{
  GstVaapiVideoAllocator *const allocator =
      GST_VAAPI_VIDEO_ALLOCATOR_CAST (GST_MEMORY_CAST (mem)->allocator);

  return
      gst_vaapi_surface_proxy_new_from_pool (GST_VAAPI_SURFACE_POOL
      (allocator->surface_pool));
}

static gboolean
ensure_surface (GstVaapiVideoMemory * mem)
{
  if (!mem->proxy) {
    gst_vaapi_surface_proxy_replace (&mem->proxy,
        gst_vaapi_video_meta_get_surface_proxy (mem->meta));

    if (!mem->proxy) {
      mem->proxy = new_surface_proxy (mem);
      if (!mem->proxy)
        return FALSE;
      gst_vaapi_video_meta_set_surface_proxy (mem->meta, mem->proxy);
    }
  }
  mem->surface = GST_VAAPI_SURFACE_PROXY_SURFACE (mem->proxy);
  return mem->surface != NULL;
}

static gboolean
ensure_surface_is_current (GstVaapiVideoMemory * mem)
{
  if (!use_native_formats (mem->usage_flag))
    return TRUE;

  if (!GST_VAAPI_VIDEO_MEMORY_FLAG_IS_SET (mem,
          GST_VAAPI_VIDEO_MEMORY_FLAG_SURFACE_IS_CURRENT)) {
    if (GST_VAAPI_VIDEO_MEMORY_FLAG_IS_SET (mem,
            GST_VAAPI_VIDEO_MEMORY_FLAG_IMAGE_IS_CURRENT)
        && !gst_vaapi_surface_put_image (mem->surface, mem->image))
      return FALSE;

    GST_VAAPI_VIDEO_MEMORY_FLAG_SET (mem,
        GST_VAAPI_VIDEO_MEMORY_FLAG_SURFACE_IS_CURRENT);
  }
  return TRUE;
}

static inline gboolean
map_vaapi_memory (GstVaapiVideoMemory * mem, GstMapFlags flags)
{
  if (!ensure_surface (mem))
    goto error_no_surface;
  if (!ensure_image (mem))
    goto error_no_image;

  /* Load VA image from surface only for read flag since it returns
   * raw pixels */
  if ((flags & GST_MAP_READ) && !ensure_image_is_current (mem))
    goto error_no_current_image;

  if (!gst_vaapi_image_map (mem->image))
    goto error_map_image;

  /* Mark surface as dirty and expect updates from image */
  if (flags & GST_MAP_WRITE)
    GST_VAAPI_VIDEO_MEMORY_FLAG_UNSET (mem,
        GST_VAAPI_VIDEO_MEMORY_FLAG_SURFACE_IS_CURRENT);

  return TRUE;

error_no_surface:
  {
    const GstVideoInfo *const vip = mem->surface_info;
    GST_ERROR ("failed to extract VA surface of size %ux%u and format %s",
        GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip),
        GST_VIDEO_INFO_FORMAT_STRING (vip));
    return FALSE;
  }
error_no_image:
  {
    const GstVideoInfo *const vip = mem->image_info;
    GST_ERROR ("failed to extract VA image of size %ux%u and format %s",
        GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip),
        GST_VIDEO_INFO_FORMAT_STRING (vip));
    return FALSE;
  }
error_no_current_image:
  {
    GST_ERROR ("failed to make image current");
    return FALSE;
  }
error_map_image:
  {
    GST_ERROR ("failed to map image %" GST_VAAPI_ID_FORMAT,
        GST_VAAPI_ID_ARGS (gst_vaapi_image_get_id (mem->image)));
    return FALSE;
  }
}

static inline void
unmap_vaapi_memory (GstVaapiVideoMemory * mem, GstMapFlags flags)
{
  gst_vaapi_image_unmap (mem->image);

  if (flags & GST_MAP_WRITE) {
    GST_VAAPI_VIDEO_MEMORY_FLAG_SET (mem,
        GST_VAAPI_VIDEO_MEMORY_FLAG_IMAGE_IS_CURRENT);
  }

  if (!use_native_formats (mem->usage_flag)) {
    gst_vaapi_video_meta_set_image (mem->meta, NULL);
    gst_vaapi_video_memory_reset_image (mem);
  }
}

gboolean
gst_video_meta_map_vaapi_memory (GstVideoMeta * meta, guint plane,
    GstMapInfo * info, gpointer * data, gint * stride, GstMapFlags flags)
{
  gboolean ret = FALSE;
  GstAllocator *allocator;
  GstVaapiVideoMemory *const mem =
      GST_VAAPI_VIDEO_MEMORY_CAST (gst_buffer_peek_memory (meta->buffer, 0));

  g_return_val_if_fail (mem, FALSE);
  g_return_val_if_fail (mem->meta, FALSE);

  allocator = GST_MEMORY_CAST (mem)->allocator;
  g_return_val_if_fail (GST_VAAPI_IS_VIDEO_ALLOCATOR (allocator), FALSE);

  g_mutex_lock (&mem->lock);
  if (mem->map_type && mem->map_type != GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_PLANAR)
    goto error_incompatible_map;

  /* Map for writing */
  if (mem->map_count == 0) {
    if (!map_vaapi_memory (mem, flags))
      goto out;
    mem->map_type = GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_PLANAR;
  }
  mem->map_count++;

  *data = gst_vaapi_image_get_plane (mem->image, plane);
  *stride = gst_vaapi_image_get_pitch (mem->image, plane);
  info->flags = flags;
  ret = (*data != NULL);

out:
  g_mutex_unlock (&mem->lock);
  return ret;

  /* ERRORS */
error_incompatible_map:
  {
    GST_ERROR ("incompatible map type (%d)", mem->map_type);
    goto out;
  }
}

gboolean
gst_video_meta_unmap_vaapi_memory (GstVideoMeta * meta, guint plane,
    GstMapInfo * info)
{
  GstAllocator *allocator;
  GstVaapiVideoMemory *const mem =
      GST_VAAPI_VIDEO_MEMORY_CAST (gst_buffer_peek_memory (meta->buffer, 0));

  g_return_val_if_fail (mem, FALSE);
  g_return_val_if_fail (mem->meta, FALSE);
  g_return_val_if_fail (mem->surface, FALSE);
  g_return_val_if_fail (mem->image, FALSE);

  allocator = GST_MEMORY_CAST (mem)->allocator;
  g_return_val_if_fail (GST_VAAPI_IS_VIDEO_ALLOCATOR (allocator), FALSE);

  g_mutex_lock (&mem->lock);
  if (--mem->map_count == 0) {
    mem->map_type = 0;

    /* Unmap VA image used for read/writes */
    if (info->flags & GST_MAP_READWRITE)
      unmap_vaapi_memory (mem, info->flags);
  }
  g_mutex_unlock (&mem->lock);
  return TRUE;
}

GstMemory *
gst_vaapi_video_memory_new (GstAllocator * base_allocator,
    GstVaapiVideoMeta * meta)
{
  GstVaapiVideoAllocator *const allocator =
      GST_VAAPI_VIDEO_ALLOCATOR_CAST (base_allocator);
  const GstVideoInfo *vip;
  GstVaapiVideoMemory *mem;

  g_return_val_if_fail (GST_VAAPI_IS_VIDEO_ALLOCATOR (allocator), NULL);

  mem = g_slice_new (GstVaapiVideoMemory);
  if (!mem)
    return NULL;

  vip = &allocator->image_info;
  gst_memory_init (&mem->parent_instance, GST_MEMORY_FLAG_NO_SHARE,
      gst_object_ref (allocator), NULL, GST_VIDEO_INFO_SIZE (vip), 0,
      0, GST_VIDEO_INFO_SIZE (vip));

  mem->proxy = NULL;
  mem->surface_info = &allocator->surface_info;
  mem->surface = NULL;
  mem->image_info = &allocator->image_info;
  mem->image = NULL;
  mem->meta = meta ? gst_vaapi_video_meta_ref (meta) : NULL;
  mem->map_type = 0;
  mem->map_count = 0;
  mem->usage_flag = allocator->usage_flag;
  g_mutex_init (&mem->lock);

  GST_VAAPI_VIDEO_MEMORY_FLAG_SET (mem,
      GST_VAAPI_VIDEO_MEMORY_FLAG_SURFACE_IS_CURRENT);
  return GST_MEMORY_CAST (mem);
}

void
gst_vaapi_video_memory_reset_image (GstVaapiVideoMemory * mem)
{
  GstVaapiVideoAllocator *const allocator =
      GST_VAAPI_VIDEO_ALLOCATOR_CAST (GST_MEMORY_CAST (mem)->allocator);

  if (!use_native_formats (mem->usage_flag))
    gst_vaapi_object_replace (&mem->image, NULL);
  else if (mem->image) {
    gst_vaapi_video_pool_put_object (allocator->image_pool, mem->image);
    mem->image = NULL;
  }

  /* Don't synchronize to surface, this shall have happened during
   * unmaps */
  GST_VAAPI_VIDEO_MEMORY_FLAG_UNSET (mem,
      GST_VAAPI_VIDEO_MEMORY_FLAG_IMAGE_IS_CURRENT);
}

void
gst_vaapi_video_memory_reset_surface (GstVaapiVideoMemory * mem)
{
  mem->surface = NULL;
  gst_vaapi_video_memory_reset_image (mem);
  gst_vaapi_surface_proxy_replace (&mem->proxy, NULL);
  if (mem->meta)
    gst_vaapi_video_meta_set_surface_proxy (mem->meta, NULL);

  GST_VAAPI_VIDEO_MEMORY_FLAG_UNSET (mem,
      GST_VAAPI_VIDEO_MEMORY_FLAG_SURFACE_IS_CURRENT);
}

gboolean
gst_vaapi_video_memory_sync (GstVaapiVideoMemory * mem)
{
  g_return_val_if_fail (mem, FALSE);

  return ensure_surface_is_current (mem);
}

static gpointer
gst_vaapi_video_memory_map (GstMemory * base_mem, gsize maxsize, guint flags)
{
  gpointer data = NULL;
  GstVaapiVideoMemory *const mem = GST_VAAPI_VIDEO_MEMORY_CAST (base_mem);

  g_return_val_if_fail (mem, NULL);
  g_return_val_if_fail (mem->meta, NULL);

  g_mutex_lock (&mem->lock);
  if (mem->map_count == 0) {
    switch (flags & GST_MAP_READWRITE) {
      case 0:
        // No flags set: return a GstVaapiSurfaceProxy
        gst_vaapi_surface_proxy_replace (&mem->proxy,
            gst_vaapi_video_meta_get_surface_proxy (mem->meta));
        if (!mem->proxy)
          goto error_no_surface_proxy;
        if (!ensure_surface_is_current (mem))
          goto error_no_current_surface;
        mem->map_type = GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_SURFACE;
        break;
      case GST_MAP_READ:
        if (!map_vaapi_memory (mem, flags))
          goto out;
        mem->map_type = GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_LINEAR;
        break;
      default:
        goto error_unsupported_map;
    }
  }

  switch (mem->map_type) {
    case GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_SURFACE:
      if (!mem->proxy)
        goto error_no_surface_proxy;
      data = mem->proxy;
      break;
    case GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_LINEAR:
      if (!mem->image)
        goto error_no_image;
      data = get_image_data (mem->image);
      break;
    default:
      goto error_unsupported_map_type;
  }
  mem->map_count++;

out:
  g_mutex_unlock (&mem->lock);
  return data;

  /* ERRORS */
error_unsupported_map:
  {
    GST_ERROR ("unsupported map flags (0x%x)", flags);
    goto out;
  }
error_unsupported_map_type:
  {
    GST_ERROR ("unsupported map type (%d)", mem->map_type);
    goto out;
  }
error_no_surface_proxy:
  {
    GST_ERROR ("failed to extract GstVaapiSurfaceProxy from video meta");
    goto out;
  }
error_no_current_surface:
  {
    GST_ERROR ("failed to make surface current");
    goto out;
  }
error_no_image:
  {
    GST_ERROR ("failed to extract VA image from video buffer");
    goto out;
  }
}

static void
gst_vaapi_video_memory_unmap_full (GstMemory * base_mem, GstMapInfo * info)
{
  GstVaapiVideoMemory *const mem = GST_VAAPI_VIDEO_MEMORY_CAST (base_mem);

  g_mutex_lock (&mem->lock);
  if (mem->map_count == 1) {
    switch (mem->map_type) {
      case GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_SURFACE:
        gst_vaapi_surface_proxy_replace (&mem->proxy, NULL);
        break;
      case GST_VAAPI_VIDEO_MEMORY_MAP_TYPE_LINEAR:
        unmap_vaapi_memory (mem, info->flags);
        break;
      default:
        goto error_incompatible_map;
    }
    mem->map_type = 0;
  }
  mem->map_count--;

out:
  g_mutex_unlock (&mem->lock);
  return;

  /* ERRORS */
error_incompatible_map:
  {
    GST_ERROR ("incompatible map type (%d)", mem->map_type);
    goto out;
  }
}

static GstMemory *
gst_vaapi_video_memory_copy (GstMemory * base_mem, gssize offset, gssize size)
{
  GstVaapiVideoMemory *const mem = GST_VAAPI_VIDEO_MEMORY_CAST (base_mem);
  GstVaapiVideoMeta *meta;
  GstAllocator *allocator;
  GstMemory *out_mem;
  gsize maxsize;

  g_return_val_if_fail (mem, NULL);
  g_return_val_if_fail (mem->meta, NULL);

  allocator = base_mem->allocator;
  g_return_val_if_fail (GST_VAAPI_IS_VIDEO_ALLOCATOR (allocator), FALSE);

  /* XXX: this implements a soft-copy, i.e. underlying VA surfaces
     are not copied */
  (void) gst_memory_get_sizes (base_mem, NULL, &maxsize);
  if (offset != 0 || (size != -1 && (gsize) size != maxsize))
    goto error_unsupported;

  if (!ensure_surface_is_current (mem))
    goto error_no_current_surface;

  meta = gst_vaapi_video_meta_copy (mem->meta);
  if (!meta)
    goto error_allocate_memory;

  out_mem = gst_vaapi_video_memory_new (allocator, meta);
  gst_vaapi_video_meta_unref (meta);
  if (!out_mem)
    goto error_allocate_memory;
  return out_mem;

  /* ERRORS */
error_no_current_surface:
  {
    GST_ERROR ("failed to make surface current");
    return NULL;
  }
error_unsupported:
  {
    GST_ERROR ("failed to copy partial memory (unsupported operation)");
    return NULL;
  }
error_allocate_memory:
  {
    GST_ERROR ("failed to allocate GstVaapiVideoMemory copy");
    return NULL;
  }
}

/* ------------------------------------------------------------------------ */
/* --- GstVaapiVideoAllocator                                           --- */
/* ------------------------------------------------------------------------ */

#define GST_VAAPI_VIDEO_ALLOCATOR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_VAAPI_TYPE_VIDEO_ALLOCATOR, \
      GstVaapiVideoAllocatorClass))

#define GST_VAAPI_IS_VIDEO_ALLOCATOR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_VAAPI_TYPE_VIDEO_ALLOCATOR))

G_DEFINE_TYPE (GstVaapiVideoAllocator, gst_vaapi_video_allocator,
    GST_TYPE_ALLOCATOR);

static void
gst_vaapi_video_allocator_free (GstAllocator * allocator, GstMemory * base_mem)
{
  GstVaapiVideoMemory *const mem = GST_VAAPI_VIDEO_MEMORY_CAST (base_mem);

  mem->surface = NULL;
  gst_vaapi_video_memory_reset_image (mem);
  gst_vaapi_surface_proxy_replace (&mem->proxy, NULL);
  gst_vaapi_video_meta_replace (&mem->meta, NULL);
  gst_object_unref (GST_MEMORY_CAST (mem)->allocator);
  g_mutex_clear (&mem->lock);
  g_slice_free (GstVaapiVideoMemory, mem);
}

static void
gst_vaapi_video_allocator_finalize (GObject * object)
{
  GstVaapiVideoAllocator *const allocator =
      GST_VAAPI_VIDEO_ALLOCATOR_CAST (object);

  gst_vaapi_video_pool_replace (&allocator->surface_pool, NULL);
  gst_vaapi_video_pool_replace (&allocator->image_pool, NULL);

  G_OBJECT_CLASS (gst_vaapi_video_allocator_parent_class)->finalize (object);
}

static void
gst_vaapi_video_allocator_class_init (GstVaapiVideoAllocatorClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass *const allocator_class = GST_ALLOCATOR_CLASS (klass);

  _init_vaapi_video_memory_debug ();

  object_class->finalize = gst_vaapi_video_allocator_finalize;
  allocator_class->free = gst_vaapi_video_allocator_free;
}

static void
gst_vaapi_video_allocator_init (GstVaapiVideoAllocator * allocator)
{
  GstAllocator *const base_allocator = GST_ALLOCATOR_CAST (allocator);

  base_allocator->mem_type = GST_VAAPI_VIDEO_MEMORY_NAME;
  base_allocator->mem_map = gst_vaapi_video_memory_map;
  base_allocator->mem_unmap_full = gst_vaapi_video_memory_unmap_full;
  base_allocator->mem_copy = gst_vaapi_video_memory_copy;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static gboolean
gst_video_info_update_from_image (GstVideoInfo * vip, GstVaapiImage * image)
{
  GstVideoFormat format;
  const guchar *data;
  guint i, num_planes, data_size, width, height;

  /* Reset format from image */
  format = gst_vaapi_image_get_format (image);
  gst_vaapi_image_get_size (image, &width, &height);
  gst_video_info_set_format (vip, format, width, height);

  num_planes = gst_vaapi_image_get_plane_count (image);
  g_return_val_if_fail (num_planes == GST_VIDEO_INFO_N_PLANES (vip), FALSE);

  /* Determine the base data pointer */
  data = get_image_data (image);
  g_return_val_if_fail (data != NULL, FALSE);
  data_size = gst_vaapi_image_get_data_size (image);

  /* Check that we don't have disjoint planes */
  for (i = 0; i < num_planes; i++) {
    const guchar *const plane = gst_vaapi_image_get_plane (image, i);
    if (plane - data > data_size)
      return FALSE;
  }

  /* Update GstVideoInfo structure */
  for (i = 0; i < num_planes; i++) {
    const guchar *const plane = gst_vaapi_image_get_plane (image, i);
    GST_VIDEO_INFO_PLANE_OFFSET (vip, i) = plane - data;
    GST_VIDEO_INFO_PLANE_STRIDE (vip, i) = gst_vaapi_image_get_pitch (image, i);
  }
  GST_VIDEO_INFO_SIZE (vip) = data_size;
  return TRUE;
}

static inline void
allocator_configure_surface_info (GstVaapiDisplay * display,
    GstVaapiVideoAllocator * allocator, GstVaapiImageUsageFlags req_usage_flag)
{
  const GstVideoInfo *vinfo;
  GstVaapiSurface *surface = NULL;
  GstVaapiImage *image = NULL;
  gboolean updated, has_direct_uploading, has_direct_rendering;
  GstVideoFormat fmt;

  vinfo = &allocator->video_info;
  allocator->usage_flag = GST_VAAPI_IMAGE_USAGE_FLAG_NATIVE_FORMATS;

  fmt = gst_vaapi_video_format_get_best_native (GST_VIDEO_INFO_FORMAT (vinfo));
  gst_video_info_set_format (&allocator->surface_info, fmt,
      GST_VIDEO_INFO_WIDTH (vinfo), GST_VIDEO_INFO_HEIGHT (vinfo));

  /* nothing to configure */
  if (use_native_formats (req_usage_flag)
      || GST_VIDEO_INFO_FORMAT (vinfo) == GST_VIDEO_FORMAT_ENCODED)
    return;

  surface = new_surface (display, vinfo, req_usage_flag);
  if (!surface)
    goto error_no_surface;
  image = gst_vaapi_surface_derive_image (surface);
  if (!image)
    goto error_no_derive_image;
  if (!gst_vaapi_image_map (image))
    goto error_cannot_map;

  updated = gst_video_info_update_from_image (&allocator->surface_info, image);

  has_direct_rendering = updated && use_direct_rendering (req_usage_flag)
      && (GST_VAAPI_IMAGE_FORMAT (image) == GST_VIDEO_INFO_FORMAT (vinfo));
  has_direct_uploading = updated && use_direct_uploading (req_usage_flag)
      && (GST_VAAPI_IMAGE_FORMAT (image) == GST_VIDEO_INFO_FORMAT (vinfo));

  gst_vaapi_image_unmap (image);

  if (has_direct_rendering && !has_direct_uploading) {
    allocator->usage_flag = GST_VAAPI_IMAGE_USAGE_FLAG_DIRECT_RENDER;
    GST_INFO_OBJECT (allocator, "has direct-rendering for %s surfaces",
        GST_VIDEO_INFO_FORMAT_STRING (&allocator->surface_info));
  } else if (!has_direct_rendering && has_direct_uploading) {
    allocator->usage_flag = GST_VAAPI_IMAGE_USAGE_FLAG_DIRECT_UPLOAD;
    GST_INFO_OBJECT (allocator, "has direct-uploading for %s surfaces",
        GST_VIDEO_INFO_FORMAT_STRING (&allocator->surface_info));
  }

bail:
  if (image)
    gst_vaapi_object_unref (image);
  if (surface)
    gst_vaapi_object_unref (surface);
  return;

error_no_surface:
  {
    GST_ERROR_OBJECT (allocator, "Cannot create a VA Surface");
    return;
  }
error_no_derive_image:
  {
    GST_ERROR_OBJECT (allocator,
        "Cannot create a derived image from surface %p", surface);
    goto bail;
  }
error_cannot_map:
  {
    GST_ERROR_OBJECT (allocator, "Cannot map VA derived image %p", image);
    goto bail;
  }
}

static inline void
allocator_configure_image_info (GstVaapiDisplay * display,
    GstVaapiVideoAllocator * allocator)
{
  GstVaapiImage *image = NULL;
  const GstVideoInfo *vinfo;

  if (!use_native_formats (allocator->usage_flag)) {
    allocator->image_info = allocator->surface_info;
    return;
  }

  vinfo = &allocator->video_info;
  allocator->image_info = *vinfo;
  gst_video_info_force_nv12_if_encoded (&allocator->image_info);

  image = new_image (display, &allocator->image_info);
  if (!image)
    goto bail;
  if (!gst_vaapi_image_map (image))
    goto bail;

  gst_video_info_update_from_image (&allocator->image_info, image);
  gst_vaapi_image_unmap (image);

bail:
  if (image)
    gst_vaapi_object_unref (image);
}

GstAllocator *
gst_vaapi_video_allocator_new (GstVaapiDisplay * display,
    const GstVideoInfo * vip, guint surface_alloc_flags,
    GstVaapiImageUsageFlags req_usage_flag)
{
  GstVaapiVideoAllocator *allocator;

  g_return_val_if_fail (display != NULL, NULL);
  g_return_val_if_fail (vip != NULL, NULL);

  allocator = g_object_new (GST_VAAPI_TYPE_VIDEO_ALLOCATOR, NULL);
  if (!allocator)
    return NULL;

  allocator->video_info = *vip;

  allocator_configure_surface_info (display, allocator, req_usage_flag);
  allocator->surface_pool = gst_vaapi_surface_pool_new_full (display,
      &allocator->surface_info, surface_alloc_flags);
  if (!allocator->surface_pool)
    goto error_create_surface_pool;

  allocator_configure_image_info (display, allocator);
  allocator->image_pool = gst_vaapi_image_pool_new (display,
      &allocator->image_info);
  if (!allocator->image_pool)
    goto error_create_image_pool;

  gst_allocator_set_vaapi_video_info (GST_ALLOCATOR_CAST (allocator),
      &allocator->image_info, surface_alloc_flags);
  return GST_ALLOCATOR_CAST (allocator);

  /* ERRORS */
error_create_surface_pool:
  {
    GST_ERROR ("failed to allocate VA surface pool");
    gst_object_unref (allocator);
    return NULL;
  }
error_create_image_pool:
  {
    GST_ERROR ("failed to allocate VA image pool");
    gst_object_unref (allocator);
    return NULL;
  }
}

/* ------------------------------------------------------------------------ */
/* --- GstVaapiDmaBufMemory                                             --- */
/* ------------------------------------------------------------------------ */

#define GST_VAAPI_BUFFER_PROXY_QUARK gst_vaapi_buffer_proxy_quark_get ()
static GQuark
gst_vaapi_buffer_proxy_quark_get (void)
{
  static gsize g_quark;

  if (g_once_init_enter (&g_quark)) {
    gsize quark = (gsize) g_quark_from_static_string ("GstVaapiBufferProxy");
    g_once_init_leave (&g_quark, quark);
  }
  return g_quark;
}

GstMemory *
gst_vaapi_dmabuf_memory_new (GstAllocator * base_allocator,
    GstVaapiVideoMeta * meta)
{
  GstMemory *mem;
  GstVaapiDisplay *display;
  GstVaapiSurface *surface;
  GstVaapiSurfaceProxy *proxy;
  GstVaapiBufferProxy *dmabuf_proxy;
  gint dmabuf_fd;
  const GstVideoInfo *vip;
  guint flags;
  GstVaapiDmaBufAllocator *const allocator =
      GST_VAAPI_DMABUF_ALLOCATOR_CAST (base_allocator);

  g_return_val_if_fail (allocator != NULL, NULL);
  g_return_val_if_fail (meta != NULL, NULL);

  vip = gst_allocator_get_vaapi_video_info (base_allocator, &flags);
  if (!vip)
    return NULL;

  display = gst_vaapi_video_meta_get_display (meta);
  if (!meta)
    return NULL;

  surface = gst_vaapi_surface_new_full (display, vip, flags);
  if (!surface)
    goto error_create_surface;

  proxy = gst_vaapi_surface_proxy_new (surface);
  if (!proxy)
    goto error_create_surface_proxy;

  dmabuf_proxy = gst_vaapi_surface_get_dma_buf_handle (surface);
  gst_vaapi_object_unref (surface);
  if (!dmabuf_proxy)
    goto error_create_dmabuf_proxy;

  gst_vaapi_video_meta_set_surface_proxy (meta, proxy);
  gst_vaapi_surface_proxy_unref (proxy);

  /* Need dup because GstDmabufMemory creates the GstFdMemory with flag
   * GST_FD_MEMORY_FLAG_NONE. So when being freed it calls close on the fd
   * because GST_FD_MEMORY_FLAG_DONT_CLOSE is not set. */
  dmabuf_fd = gst_vaapi_buffer_proxy_get_handle (dmabuf_proxy);
  if (dmabuf_fd < 0 || (dmabuf_fd = dup (dmabuf_fd)) < 0)
    goto error_create_dmabuf_handle;

  mem = gst_dmabuf_allocator_alloc (base_allocator, dmabuf_fd,
      gst_vaapi_buffer_proxy_get_size (dmabuf_proxy));
  if (!mem)
    goto error_create_dmabuf_memory;

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (mem),
      GST_VAAPI_BUFFER_PROXY_QUARK, dmabuf_proxy,
      (GDestroyNotify) gst_vaapi_buffer_proxy_unref);
  return mem;

  /* ERRORS */
error_create_surface:
  {
    GST_ERROR ("failed to create VA surface (format:%s size:%ux%u)",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (vip)),
        GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip));
    return NULL;
  }
error_create_surface_proxy:
  {
    GST_ERROR ("failed to create VA surface proxy");
    gst_vaapi_object_unref (surface);
    return NULL;
  }
error_create_dmabuf_proxy:
  {
    GST_ERROR ("failed to export VA surface to DMABUF");
    gst_vaapi_surface_proxy_unref (proxy);
    return NULL;
  }
error_create_dmabuf_handle:
  {
    GST_ERROR ("failed to duplicate DMABUF handle");
    gst_vaapi_buffer_proxy_unref (dmabuf_proxy);
    return NULL;
  }
error_create_dmabuf_memory:
  {
    GST_ERROR ("failed to create DMABUF memory");
    gst_vaapi_buffer_proxy_unref (dmabuf_proxy);
    return NULL;
  }
}

/* ------------------------------------------------------------------------ */
/* --- GstVaapiDmaBufAllocator                                          --- */
/* ------------------------------------------------------------------------ */

#define GST_VAAPI_DMABUF_ALLOCATOR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_VAAPI_TYPE_DMABUF_ALLOCATOR, \
      GstVaapiDmaBufAllocatorClass))

#define GST_VAAPI_IS_DMABUF_ALLOCATOR_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_VAAPI_TYPE_DMABUF_ALLOCATOR))

G_DEFINE_TYPE (GstVaapiDmaBufAllocator,
    gst_vaapi_dmabuf_allocator, GST_TYPE_DMABUF_ALLOCATOR);

static void
gst_vaapi_dmabuf_allocator_class_init (GstVaapiDmaBufAllocatorClass * klass)
{
}

static void
gst_vaapi_dmabuf_allocator_init (GstVaapiDmaBufAllocator * allocator)
{
  GstAllocator *const base_allocator = GST_ALLOCATOR_CAST (allocator);

  base_allocator->mem_type = GST_VAAPI_DMABUF_ALLOCATOR_NAME;
}

GstAllocator *
gst_vaapi_dmabuf_allocator_new (GstVaapiDisplay * display,
    const GstVideoInfo * vip, guint flags)
{
  GstVaapiDmaBufAllocator *allocator = NULL;
  GstVaapiSurface *surface = NULL;
  GstVaapiImage *image = NULL;
  GstVideoInfo alloc_info;

  g_return_val_if_fail (display != NULL, NULL);
  g_return_val_if_fail (vip != NULL, NULL);

  _init_vaapi_video_memory_debug ();

  surface = gst_vaapi_surface_new_full (display, vip, flags);
  if (!surface)
    goto error_no_surface;

  image = gst_vaapi_surface_derive_image (surface);
  if (!image)
    goto error_no_image;
  if (!gst_vaapi_image_map (image))
    goto error_map_failed;

  gst_video_info_set_format (&alloc_info, GST_VIDEO_INFO_FORMAT (vip),
      GST_VIDEO_INFO_WIDTH (vip), GST_VIDEO_INFO_HEIGHT (vip));
  gst_video_info_update_from_image (&alloc_info, image);
  gst_vaapi_image_unmap (image);

  allocator = g_object_new (GST_VAAPI_TYPE_DMABUF_ALLOCATOR, NULL);
  if (!allocator)
    goto error_no_allocator;
  gst_allocator_set_vaapi_video_info (GST_ALLOCATOR_CAST (allocator),
      &alloc_info, flags);

bail:
  gst_vaapi_object_replace (&image, NULL);
  gst_vaapi_object_replace (&surface, NULL);
  return GST_ALLOCATOR_CAST (allocator);

  /* ERRORS */
error_no_surface:
  {
    GST_ERROR ("failed to create a new surface");
    goto bail;
  }
error_no_image:
  {
    GST_ERROR ("failed derive surface to image for format: %s",
        GST_VIDEO_INFO_FORMAT_STRING (vip));
    goto bail;
  }
error_map_failed:
  {
    GST_ERROR ("failed to map image");
    goto bail;
  }
error_no_allocator:
  {
    GST_ERROR ("failed to create a new dmabuf allocator");
    goto bail;
  }
}

/* ------------------------------------------------------------------------ */
/* --- GstVaapiVideoInfo = { GstVideoInfo, flags }                      --- */
/* ------------------------------------------------------------------------ */

static GstVideoInfo *
gst_vaapi_video_info_copy (const GstVideoInfo * vip)
{
  GstVideoInfo *out_vip;

  out_vip = g_slice_new (GstVideoInfo);
  if (!out_vip)
    return NULL;

  gst_video_info_init (out_vip);
  *out_vip = *vip;
  return out_vip;
}

static void
gst_vaapi_video_info_free (GstVideoInfo * vip)
{
  g_slice_free (GstVideoInfo, vip);
}

#define GST_VAAPI_TYPE_VIDEO_INFO gst_vaapi_video_info_get_type ()
static GType
gst_vaapi_video_info_get_type (void)
{
  static gsize g_type;

  if (g_once_init_enter (&g_type)) {
    GType type;
    type = g_boxed_type_register_static ("GstVaapiVideoInfo",
        (GBoxedCopyFunc) gst_vaapi_video_info_copy,
        (GBoxedFreeFunc) gst_vaapi_video_info_free);
    g_once_init_leave (&g_type, type);
  }
  return (GType) g_type;
}

#define GST_VAAPI_VIDEO_INFO_QUARK gst_vaapi_video_info_quark_get ()
static GQuark
gst_vaapi_video_info_quark_get (void)
{
  static gsize g_quark;

  if (g_once_init_enter (&g_quark)) {
    gsize quark = (gsize) g_quark_from_static_string ("GstVaapiVideoInfo");
    g_once_init_leave (&g_quark, quark);
  }
  return g_quark;
}

#define INFO_QUARK info_quark_get ()
static GQuark
info_quark_get (void)
{
  static gsize g_quark;

  if (g_once_init_enter (&g_quark)) {
    gsize quark = (gsize) g_quark_from_static_string ("info");
    g_once_init_leave (&g_quark, quark);
  }
  return g_quark;
}

#define FLAGS_QUARK flags_quark_get ()
static GQuark
flags_quark_get (void)
{
  static gsize g_quark;

  if (g_once_init_enter (&g_quark)) {
    gsize quark = (gsize) g_quark_from_static_string ("flags");
    g_once_init_leave (&g_quark, quark);
  }
  return g_quark;
}

const GstVideoInfo *
gst_allocator_get_vaapi_video_info (GstAllocator * allocator,
    guint * out_flags_ptr)
{
  const GstStructure *structure;
  const GValue *value;

  g_return_val_if_fail (GST_IS_ALLOCATOR (allocator), NULL);

  structure =
      g_object_get_qdata (G_OBJECT (allocator), GST_VAAPI_VIDEO_INFO_QUARK);
  if (!structure)
    return NULL;

  if (out_flags_ptr) {
    value = gst_structure_id_get_value (structure, FLAGS_QUARK);
    if (!value)
      return NULL;
    *out_flags_ptr = g_value_get_uint (value);
  }

  value = gst_structure_id_get_value (structure, INFO_QUARK);
  if (!value)
    return NULL;
  return g_value_get_boxed (value);
}

gboolean
gst_allocator_set_vaapi_video_info (GstAllocator * allocator,
    const GstVideoInfo * vip, guint flags)
{
  g_return_val_if_fail (GST_IS_ALLOCATOR (allocator), FALSE);
  g_return_val_if_fail (vip != NULL, FALSE);

  g_object_set_qdata_full (G_OBJECT (allocator), GST_VAAPI_VIDEO_INFO_QUARK,
      gst_structure_new_id (GST_VAAPI_VIDEO_INFO_QUARK,
          INFO_QUARK, GST_VAAPI_TYPE_VIDEO_INFO, vip,
          FLAGS_QUARK, G_TYPE_UINT, flags, NULL),
      (GDestroyNotify) gst_structure_free);

  return TRUE;
}

/**
 * gst_allocator_get_vaapi_image_size:
 * @allocator: a #GstAllocator instance.
 * @size: (out) (optional): the VA image size created by @allocator.
 *
 * This function gets the size of the VA images instantiated by the
 * @allocator.
 *
 * Returns: %TRUE if @allocator is VA valid
 **/
gboolean
gst_allocator_get_vaapi_image_size (GstAllocator * allocator, guint * size)
{
  GstVaapiVideoAllocator *alloc;

  g_return_val_if_fail (GST_IS_ALLOCATOR (allocator), FALSE);

  if (g_strcmp0 (allocator->mem_type, GST_VAAPI_VIDEO_MEMORY_NAME))
    return FALSE;
  alloc = GST_VAAPI_VIDEO_ALLOCATOR_CAST (allocator);
  if (alloc && size)
    *size = GST_VIDEO_INFO_SIZE (&alloc->image_info);

  return TRUE;
}

gboolean
gst_vaapi_is_dmabuf_allocator (GstAllocator * allocator)
{
  GstStructure *st;

  g_return_val_if_fail (GST_IS_ALLOCATOR (allocator), FALSE);

  if (g_strcmp0 (allocator->mem_type, GST_VAAPI_DMABUF_ALLOCATOR_NAME) != 0)
    return FALSE;
  st = g_object_get_qdata (G_OBJECT (allocator), GST_VAAPI_VIDEO_INFO_QUARK);
  return (st != NULL);
}
