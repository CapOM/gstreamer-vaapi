/*
 *  gstvaapidisplay_priv.h - Base VA display (private definitions)
 *
 *  Copyright (C) 2010-2011 Splitted-Desktop Systems
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@splitted-desktop.com>
 *  Copyright (C) 2011-2013 Intel Corporation
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

#ifndef GST_VAAPI_DISPLAY_PRIV_H
#define GST_VAAPI_DISPLAY_PRIV_H

#include <gst/vaapi/gstvaapidisplay.h>
#include <gst/vaapi/gstvaapidisplaycache.h>
#include <gst/vaapi/gstvaapiwindow.h>
#include <gst/vaapi/gstvaapitexture.h>
#include "gstvaapiminiobject.h"

G_BEGIN_DECLS

#define GST_VAAPI_DISPLAY_CAST(display) \
  ((GstVaapiDisplay *) (display))

#define GST_VAAPI_DISPLAY_GET_PRIVATE(display) \
  (&GST_VAAPI_DISPLAY_CAST (display)->priv)

#define GST_VAAPI_DISPLAY_CLASS(klass) \
  ((GstVaapiDisplayClass *) (klass))

#define GST_VAAPI_IS_DISPLAY_CLASS(klass) \
  ((klass) != NULL)

#define GST_VAAPI_DISPLAY_GET_CLASS(obj) \
  GST_VAAPI_DISPLAY_CLASS (GST_VAAPI_MINI_OBJECT_GET_CLASS (obj))

typedef struct _GstVaapiDisplayPrivate          GstVaapiDisplayPrivate;
typedef struct _GstVaapiDisplayClass            GstVaapiDisplayClass;
typedef enum _GstVaapiDisplayInitType           GstVaapiDisplayInitType;

typedef void (*GstVaapiDisplayInitFunc) (GstVaapiDisplay * display);
typedef gboolean (*GstVaapiDisplayBindFunc) (GstVaapiDisplay * display,
    gpointer native_dpy);
typedef gboolean (*GstVaapiDisplayOpenFunc) (GstVaapiDisplay * display,
    const gchar * name);
typedef void (*GstVaapiDisplayCloseFunc) (GstVaapiDisplay * display);
typedef void (*GstVaapiDisplayLockFunc) (GstVaapiDisplay * display);
typedef void (*GstVaapiDisplayUnlockFunc) (GstVaapiDisplay * display);
typedef void (*GstVaapiDisplaySyncFunc) (GstVaapiDisplay * display);
typedef void (*GstVaapiDisplayFlushFunc) (GstVaapiDisplay * display);
typedef gboolean (*GstVaapiDisplayGetInfoFunc) (GstVaapiDisplay * display,
    GstVaapiDisplayInfo * info);
typedef void (*GstVaapiDisplayGetSizeFunc) (GstVaapiDisplay * display,
    guint * pwidth, guint * pheight);
typedef void (*GstVaapiDisplayGetSizeMFunc) (GstVaapiDisplay * display,
    guint * pwidth, guint * pheight);
typedef GstVaapiWindow *(*GstVaapiDisplayCreateWindowFunc) (
    GstVaapiDisplay * display, GstVaapiID id, guint width, guint height);
typedef GstVaapiTexture *(*GstVaapiDisplayCreateTextureFunc) (
    GstVaapiDisplay * display, GstVaapiID id, guint target, guint format,
    guint width, guint height);

typedef guintptr (*GstVaapiDisplayGetVisualIdFunc) (GstVaapiDisplay * display,
    GstVaapiWindow * window);
typedef guintptr (*GstVaapiDisplayGetColormapFunc) (GstVaapiDisplay * display,
    GstVaapiWindow * window);

/**
 * GST_VAAPI_DISPLAY_GET_CLASS_TYPE:
 * @display: a #GstVaapiDisplay
 *
 * Returns the #display class type
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_GET_CLASS_TYPE
#define GST_VAAPI_DISPLAY_GET_CLASS_TYPE(display) \
  (GST_VAAPI_DISPLAY_GET_CLASS (display)->display_type)

/**
 * GST_VAAPI_DISPLAY_NATIVE:
 * @display: a #GstVaapiDisplay
 *
 * Macro that evaluates to the native display of @display.
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_NATIVE
#define GST_VAAPI_DISPLAY_NATIVE(display) \
  (GST_VAAPI_DISPLAY_GET_PRIVATE (display)->native_display)

/**
 * GST_VAAPI_DISPLAY_VADISPLAY:
 * @display_: a #GstVaapiDisplay
 *
 * Macro that evaluates to the #VADisplay of @display_.
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_VADISPLAY
#define GST_VAAPI_DISPLAY_VADISPLAY(display_) \
  (GST_VAAPI_DISPLAY_GET_PRIVATE (display_)->display)

/**
 * GST_VAAPI_DISPLAY_VADISPLAY_TYPE:
 * @display: a #GstVaapiDisplay
 *
 * Returns the underlying VADisplay @display type
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_VADISPLAY_TYPE
#define GST_VAAPI_DISPLAY_VADISPLAY_TYPE(display) \
  (GST_VAAPI_DISPLAY_GET_PRIVATE (display)->display_type)

/**
 * GST_VAAPI_DISPLAY_HAS_VPP:
 * @display: a @GstVaapiDisplay
 *
 * Returns whether the @display supports video processing (VA/VPP)
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_HAS_VPP
#define GST_VAAPI_DISPLAY_HAS_VPP(display) \
  gst_vaapi_display_has_video_processing (GST_VAAPI_DISPLAY_CAST (display))

/**
 * GST_VAAPI_DISPLAY_CACHE:
 * @display: a @GstVaapiDisplay
 *
 * Returns the #GstVaapiDisplayCache attached to the supplied @display object.
 * This is an internal macro that does not do any run-time type check.
 */
#undef  GST_VAAPI_DISPLAY_CACHE
#define GST_VAAPI_DISPLAY_CACHE(display) \
  (GST_VAAPI_DISPLAY_GET_PRIVATE (display)->cache)

struct _GstVaapiDisplayPrivate
{
  GstVaapiDisplay *parent;
  GstVaapiDisplayCache *cache;
  GRecMutex mutex;
  GstVaapiDisplayType display_type;
  gchar *display_name;
  VADisplay display;
  gpointer native_display;
  guint width;
  guint height;
  guint width_mm;
  guint height_mm;
  guint par_n;
  guint par_d;
  GArray *decoders;
  GArray *encoders;
  GArray *image_formats;
  GArray *subpicture_formats;
  GArray *properties;
  gchar *vendor_string;
  guint use_foreign_display:1;
  guint has_vpp:1;
  guint has_profiles:1;
};

/**
 * GstVaapiDisplay:
 *
 * Base class for VA displays.
 */
struct _GstVaapiDisplay
{
  /*< private >*/
  GstVaapiMiniObject parent_instance;

  GstVaapiDisplayPrivate priv;
};

/**
 * GstVaapiDisplayClass:
 * @open_display: virtual function to open a display
 * @close_display: virtual function to close a display
 * @lock: (optional) virtual function to lock a display
 * @unlock: (optional) virtual function to unlock a display
 * @sync: (optional) virtual function to sync a display
 * @flush: (optional) virtual function to flush pending requests of a display
 * @get_display: virtual function to retrieve the #GstVaapiDisplayInfo
 * @get_size: virtual function to retrieve the display dimensions, in pixels
 * @get_size_mm: virtual function to retrieve the display dimensions, in millimeters
 * @get_visual_id: (optional) virtual function to retrieve the window visual id
 * @get_colormap: (optional) virtual function to retrieve the window colormap
 * @create_window: (optional) virtual function to create a window
 * @create_texture: (optional) virtual function to create a texture
 *
 * Base class for VA displays.
 */
struct _GstVaapiDisplayClass
{
  /*< private >*/
  GstVaapiMiniObjectClass parent_class;

  /*< protected >*/
  guint display_type;

  /*< public >*/
  GstVaapiDisplayInitFunc init;
  GstVaapiDisplayBindFunc bind_display;
  GstVaapiDisplayOpenFunc open_display;
  GstVaapiDisplayCloseFunc close_display;
  GstVaapiDisplayLockFunc lock;
  GstVaapiDisplayUnlockFunc unlock;
  GstVaapiDisplaySyncFunc sync;
  GstVaapiDisplayFlushFunc flush;
  GstVaapiDisplayGetInfoFunc get_display;
  GstVaapiDisplayGetSizeFunc get_size;
  GstVaapiDisplayGetSizeMFunc get_size_mm;
  GstVaapiDisplayGetVisualIdFunc get_visual_id;
  GstVaapiDisplayGetColormapFunc get_colormap;
  GstVaapiDisplayCreateWindowFunc create_window;
  GstVaapiDisplayCreateTextureFunc create_texture;
};

/* Initialization types */
enum _GstVaapiDisplayInitType
{
  GST_VAAPI_DISPLAY_INIT_FROM_DISPLAY_NAME = 1,
  GST_VAAPI_DISPLAY_INIT_FROM_NATIVE_DISPLAY,
  GST_VAAPI_DISPLAY_INIT_FROM_VA_DISPLAY
};

void
gst_vaapi_display_class_init (GstVaapiDisplayClass * klass);

GstVaapiDisplay *
gst_vaapi_display_new (const GstVaapiDisplayClass * klass,
    GstVaapiDisplayInitType init_type, gpointer init_value);

/* Inline reference counting for core libgstvaapi library */
#ifdef IN_LIBGSTVAAPI_CORE
#define gst_vaapi_display_ref_internal(display) \
    ((gpointer)gst_vaapi_mini_object_ref(GST_VAAPI_MINI_OBJECT(display)))

#define gst_vaapi_display_unref_internal(display) \
    gst_vaapi_mini_object_unref(GST_VAAPI_MINI_OBJECT(display))

#define gst_vaapi_display_replace_internal(old_display_ptr, new_display) \
    gst_vaapi_mini_object_replace((GstVaapiMiniObject **)(old_display_ptr), \
        GST_VAAPI_MINI_OBJECT(new_display))

#undef  gst_vaapi_display_ref
#define gst_vaapi_display_ref(display) \
    gst_vaapi_display_ref_internal((display))

#undef  gst_vaapi_display_unref
#define gst_vaapi_display_unref(display) \
    gst_vaapi_display_unref_internal((display))

#undef  gst_vaapi_display_replace
#define gst_vaapi_display_replace(old_display_ptr, new_display) \
    gst_vaapi_display_replace_internal((old_display_ptr), (new_display))
#endif

G_END_DECLS

#endif /* GST_VAAPI_DISPLAY_PRIV_H */
