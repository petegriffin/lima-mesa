/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/u_memory.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_transfer.h"
#include "util/u_surface.h"
#include "util/hash_table.h"
#include "renderonly/renderonly.h"

#include "state_tracker/drm_driver.h"

#include "drm_fourcc.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_resource.h"
#include "lima_bo.h"
#include "lima_util.h"
#include "lima_drm.h"
#include "lima_tiling.h"

static struct pipe_resource *
lima_resource_create_scanout(struct pipe_screen *pscreen,
                             const struct pipe_resource *templat,
                             unsigned width, unsigned height)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct renderonly_scanout *scanout;
   struct winsys_handle handle;
   struct pipe_resource *pres;

   struct pipe_resource scanout_templat = *templat;
   scanout_templat.width0 = width;
   scanout_templat.height0 = height;
   scanout_templat.screen = pscreen;

   scanout = renderonly_scanout_for_resource(&scanout_templat,
                                             screen->ro, &handle);
   if (!scanout)
      return NULL;

   assert(handle.type == WINSYS_HANDLE_TYPE_FD);
   pres = pscreen->resource_from_handle(pscreen, templat, &handle,
                                        PIPE_HANDLE_USAGE_WRITE);

   close(handle.handle);
   if (!pres) {
      renderonly_scanout_destroy(scanout, screen->ro);
      return NULL;
   }

   struct lima_resource *res = lima_resource(pres);
   res->scanout = scanout;

   return pres;
}

static struct pipe_resource *
lima_resource_create_bo(struct pipe_screen *pscreen,
                        const struct pipe_resource *templat,
                        unsigned width, unsigned height)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res;
   struct pipe_resource *pres;

   res = CALLOC_STRUCT(lima_resource);
   if (!res)
      return NULL;

   res->base = *templat;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);

   /* TODO: mipmap */
   pres = &res->base;
   res->width = width;
   res->stride = util_format_get_stride(pres->format, width);

   uint32_t size = res->stride *
      util_format_get_nblocksy(pres->format, height) *
      pres->array_size * pres->depth0;
   size = align(size, LIMA_PAGE_SIZE);

   res->bo = lima_bo_create(screen, size, 0, false, false);
   if (!res->bo) {
      FREE(res);
      return NULL;
   }

   return pres;
}

static bool
is_modifier_ok(const uint64_t *modifiers, int count, uint64_t mod)
{
   if (!count)
      return true;

   for (int i = 0; i < count; i++) {
      if (modifiers[i] == mod)
         return true;
   }

   return false;
}

static struct pipe_resource *
_lima_resource_create_with_modifiers(struct pipe_screen *pscreen,
                                     const struct pipe_resource *templat,
                                     const uint64_t *modifiers,
                                     int count)
{
   struct lima_screen *screen = lima_screen(pscreen);
   bool should_tile = true;
   unsigned width, height;

   /* VBOs/PBOs are untiled (and 1 height). */
   if (templat->target == PIPE_BUFFER)
      should_tile = false;

   if (templat->bind & (PIPE_BIND_LINEAR | PIPE_BIND_SCANOUT))
      should_tile = false;

   /* if tiled buffer is not allowed, fallback to linear buffer */
   if (should_tile && !is_modifier_ok(modifiers, count, DRM_FORMAT_MOD_ARM_TILED))
      should_tile = false;

   /* if linear buffer is not allowd, alloc fail */
   if (!should_tile && !is_modifier_ok(modifiers, count, DRM_FORMAT_MOD_LINEAR))
      return NULL;

   if (should_tile || (templat->bind & PIPE_BIND_RENDER_TARGET)) {
      width = align(templat->width0, 16);
      height = align(templat->height0, 16);
   }
   else {
      width = templat->width0;
      height = templat->height0;
   }

   struct pipe_resource *pres;
   if (screen->ro && (templat->bind & PIPE_BIND_SCANOUT))
      pres = lima_resource_create_scanout(pscreen, templat, width, height);
   else
      pres = lima_resource_create_bo(pscreen, templat, width, height);

   if (pres) {
      struct lima_resource *res = lima_resource(pres);
      res->tiled = should_tile;

      if (should_tile)
         lima_bo_set_modifier(res->bo, DRM_FORMAT_MOD_ARM_TILED);
      else
         lima_bo_set_modifier(res->bo, DRM_FORMAT_MOD_LINEAR);

      debug_printf("%s: pres=%p width=%u height=%u depth=%u target=%d "
                   "bind=%x usage=%d tile=%d\n", __func__,
                   pres, pres->width0, pres->height0, pres->depth0,
                   pres->target, pres->bind, pres->usage, should_tile);
   }
   return pres;
}

static struct pipe_resource *
lima_resource_create(struct pipe_screen *pscreen,
                     const struct pipe_resource *templat)
{
   return _lima_resource_create_with_modifiers(pscreen, templat, NULL, 0);
}

static struct pipe_resource *
lima_resource_create_with_modifiers(struct pipe_screen *pscreen,
                                    const struct pipe_resource *templat,
                                    const uint64_t *modifiers,
                                    int count)
{
   struct pipe_resource tmpl = *templat;

   /* gbm_bo_create_with_modifiers & gbm_surface_create_with_modifiers
    * don't have usage parameter, but buffer created by these functions
    * may be used for scanout. So we assume buffer created by this
    * function always enable scanout if linear modifier is permitted.
    */
   if (is_modifier_ok(modifiers, count, DRM_FORMAT_MOD_LINEAR))
      tmpl.bind |= PIPE_BIND_SCANOUT;

   return _lima_resource_create_with_modifiers(pscreen, &tmpl, modifiers, count);
}

static void
lima_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res = lima_resource(pres);

   if (res->bo)
      lima_bo_free(res->bo);

   if (res->scanout)
      renderonly_scanout_destroy(res->scanout, screen->ro);

   FREE(res);
}

static struct pipe_resource *
lima_resource_from_handle(struct pipe_screen *pscreen,
        const struct pipe_resource *templat,
        struct winsys_handle *handle, unsigned usage)
{
   struct lima_resource *res;
   struct lima_screen *screen = lima_screen(pscreen);

   res = CALLOC_STRUCT(lima_resource);
   if (!res)
      return NULL;

   struct pipe_resource *pres = &res->base;
   *pres = *templat;
   pres->screen = pscreen;
   pipe_reference_init(&pres->reference, 1);
   res->stride = handle->stride;

   res->bo = lima_bo_import(screen, handle);
   if (!res->bo) {
      FREE(res);
      return NULL;
   }

   /* check alignment for the buffer */
   if (pres->bind & PIPE_BIND_RENDER_TARGET) {
      unsigned width, height, stride, size;

      width = align(pres->width0, 16);
      height = align(pres->height0, 16);
      stride = util_format_get_stride(pres->format, width);
      size = util_format_get_2d_size(pres->format, stride, height);

      if (res->stride != stride || res->bo->size < size) {
         debug_error("import buffer not properly aligned\n");
         goto err_out;
      }

      res->width = width;
   }
   else
      res->width = pres->width0;

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   lima_bo_get_modifier(res->bo, &modifier);
   if (modifier == DRM_FORMAT_MOD_INVALID) {
      if (handle->modifier == DRM_FORMAT_MOD_INVALID)
         handle->modifier = DRM_FORMAT_MOD_LINEAR;
      lima_bo_set_modifier(res->bo, handle->modifier);
   }
   else {
      if (handle->modifier == DRM_FORMAT_MOD_INVALID)
         handle->modifier = modifier;
      else if (handle->modifier != modifier) {
         debug_error("import buffer modifier mismatch\n");
         goto err_out;
      }
   }

   if (handle->modifier == DRM_FORMAT_MOD_ARM_TILED)
      res->tiled = true;
   else if (handle->modifier != DRM_FORMAT_MOD_LINEAR) {
      debug_error("import buffer with unsupport modifier\n");
      goto err_out;
   }

   return pres;

err_out:
   lima_resource_destroy(pscreen, pres);
   return NULL;
}

static boolean
lima_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *pctx,
                         struct pipe_resource *pres,
                         struct winsys_handle *handle, unsigned usage)
{
   struct lima_screen *screen = lima_screen(pscreen);
   struct lima_resource *res = lima_resource(pres);

   if (res->tiled)
      handle->modifier = DRM_FORMAT_MOD_ARM_TILED;
   else
      handle->modifier = DRM_FORMAT_MOD_LINEAR;

   if (handle->type == WINSYS_HANDLE_TYPE_KMS && screen->ro &&
       renderonly_get_handle(res->scanout, handle))
      return TRUE;

   if (!lima_bo_export(res->bo, handle))
      return FALSE;

   handle->stride = res->stride;
   return TRUE;
}

void
lima_resource_screen_init(struct lima_screen *screen)
{
   screen->base.resource_create = lima_resource_create;
   screen->base.resource_create_with_modifiers = lima_resource_create_with_modifiers;
   screen->base.resource_from_handle = lima_resource_from_handle;
   screen->base.resource_destroy = lima_resource_destroy;
   screen->base.resource_get_handle = lima_resource_get_handle;
}

static struct pipe_surface *
lima_surface_create(struct pipe_context *pctx,
                    struct pipe_resource *pres,
                    const struct pipe_surface *surf_tmpl)
{
   struct lima_surface *surf = CALLOC_STRUCT(lima_surface);

   if (!surf)
      return NULL;

   assert(surf_tmpl->u.tex.first_layer == surf_tmpl->u.tex.last_layer);

   struct pipe_surface *psurf = &surf->base;
   unsigned level = surf_tmpl->u.tex.level;

   pipe_reference_init(&psurf->reference, 1);
   pipe_resource_reference(&psurf->texture, pres);

   psurf->context = pctx;
   psurf->format = surf_tmpl->format;
   psurf->width = u_minify(pres->width0, level);
   psurf->height = u_minify(pres->height0, level);
   psurf->u.tex.level = level;
   psurf->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
   psurf->u.tex.last_layer = surf_tmpl->u.tex.last_layer;

   surf->tiled_w = align(psurf->width, 16) >> 4;
   surf->tiled_h = align(psurf->height, 16) >> 4;

   struct lima_context *ctx = lima_context(pctx);
   if (ctx->plb_pp_stream) {
      struct lima_ctx_plb_pp_stream_key key = {
         .tiled_w = surf->tiled_w,
         .tiled_h = surf->tiled_h,
      };

      for (int i = 0; i < lima_ctx_num_plb; i++) {
         key.plb_index = i;

         struct hash_entry *entry =
            _mesa_hash_table_search(ctx->plb_pp_stream, &key);
         if (entry) {
            struct lima_ctx_plb_pp_stream *s = entry->data;
            s->refcnt++;
         }
         else {
            struct lima_ctx_plb_pp_stream *s =
               ralloc(ctx->plb_pp_stream, struct lima_ctx_plb_pp_stream);
            s->key.plb_index = i;
            s->key.tiled_w = surf->tiled_w;
            s->key.tiled_h = surf->tiled_h;
            s->refcnt = 1;
            s->bo = NULL;
            _mesa_hash_table_insert(ctx->plb_pp_stream, &s->key, s);
         }
      }
   }

   debug_printf("%s: pres=%p psurf=%p\n", __func__, pres, psurf);

   return &surf->base;
}

static void
lima_surface_destroy(struct pipe_context *pctx, struct pipe_surface *psurf)
{
   struct lima_surface *surf = lima_surface(psurf);
   struct lima_context *ctx = lima_context(pctx);

   if (ctx->plb_pp_stream) {
      struct lima_ctx_plb_pp_stream_key key = {
         .tiled_w = surf->tiled_w,
         .tiled_h = surf->tiled_h,
      };

      for (int i = 0; i < lima_ctx_num_plb; i++) {
         key.plb_index = i;

         struct hash_entry *entry =
            _mesa_hash_table_search(ctx->plb_pp_stream, &key);
         struct lima_ctx_plb_pp_stream *s = entry->data;
         if (--s->refcnt == 0) {
            if (s->bo)
               lima_bo_free(s->bo);
            _mesa_hash_table_remove(ctx->plb_pp_stream, entry);
            ralloc_free(s);
         }
      }
   }

   pipe_resource_reference(&psurf->texture, NULL);
   FREE(surf);
}

static void *
lima_transfer_map(struct pipe_context *pctx,
                  struct pipe_resource *pres,
                  unsigned level,
                  unsigned usage,
                  const struct pipe_box *box,
                  struct pipe_transfer **pptrans)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_resource *res = lima_resource(pres);
   struct lima_bo *bo = res->bo;
   struct lima_transfer *trans;
   struct pipe_transfer *ptrans;

   debug_printf("%s: pres=%p\n", __func__, pres);

   /* No direct mappings of tiled, since we need to manually
    * tile/untile.
    */
   if (res->tiled && (usage & PIPE_TRANSFER_MAP_DIRECTLY))
      return NULL;

   /* use once buffers are made sure to not read/write overlapped
    * range, so no need to sync */
   if (pres->usage != PIPE_USAGE_STREAM) {
      if (usage & PIPE_TRANSFER_READ_WRITE) {
         if (lima_need_flush(ctx, bo, usage & PIPE_TRANSFER_WRITE))
            lima_flush(ctx);

         unsigned op = usage & PIPE_TRANSFER_WRITE ?
            LIMA_GEM_WAIT_WRITE : LIMA_GEM_WAIT_READ;
         lima_bo_wait(bo, op, PIPE_TIMEOUT_INFINITE);
      }
   }

   if (!lima_bo_update(bo, true, false))
      return NULL;

   trans = slab_alloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;

   memset(trans, 0, sizeof(*trans));
   ptrans = &trans->base;

   pipe_resource_reference(&ptrans->resource, pres);
   ptrans->level = level;
   ptrans->usage = usage;
   ptrans->box = *box;

   *pptrans = ptrans;

   if (res->tiled) {
      ptrans->stride = util_format_get_stride(pres->format, ptrans->box.width);
      ptrans->layer_stride = ptrans->stride * ptrans->box.height;

      trans->staging = malloc(ptrans->stride * ptrans->box.height * ptrans->box.depth);

      if (usage & PIPE_TRANSFER_READ)
         lima_load_tiled_image(trans->staging, bo->map,
                              &ptrans->box,
                              ptrans->stride,
                              res->stride,
                              util_format_get_blocksize(pres->format));

      return trans->staging;
   } else {
      ptrans->stride = res->stride;
      ptrans->layer_stride = ptrans->stride * box->height;

      return bo->map + box->z * ptrans->layer_stride +
         box->y / util_format_get_blockheight(pres->format) * ptrans->stride +
         box->x / util_format_get_blockwidth(pres->format) *
         util_format_get_blocksize(pres->format);
   }
}

static void
lima_transfer_flush_region(struct pipe_context *pctx,
                           struct pipe_transfer *ptrans,
                           const struct pipe_box *box)
{
   debug_checkpoint();
}

static void
lima_transfer_unmap(struct pipe_context *pctx,
                    struct pipe_transfer *ptrans)
{
   struct lima_context *ctx = lima_context(pctx);
   struct lima_transfer *trans = lima_transfer(ptrans);
   struct lima_resource *res = lima_resource(ptrans->resource);
   struct lima_bo *bo = res->bo;
   struct pipe_resource *pres;

   if (trans->staging) {
      pres = &res->base;
      if (ptrans->usage & PIPE_TRANSFER_WRITE)
         lima_store_tiled_image(bo->map, trans->staging,
                              &ptrans->box,
                              res->stride,
                              ptrans->stride,
                              util_format_get_blocksize(pres->format));
      free(trans->staging);
   }

   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, trans);
}

static void
lima_blit(struct pipe_context *pctx, const struct pipe_blit_info *blit_info)
{
   debug_checkpoint();
}

static void
lima_flush_resource(struct pipe_context *pctx, struct pipe_resource *resource)
{
   debug_checkpoint();

   debug_printf("flush res=%p\n", resource);
}

void
lima_resource_context_init(struct lima_context *ctx)
{
   ctx->base.create_surface = lima_surface_create;
   ctx->base.surface_destroy = lima_surface_destroy;

   ctx->base.buffer_subdata = u_default_buffer_subdata;

   ctx->base.transfer_map = lima_transfer_map;
   ctx->base.transfer_flush_region = lima_transfer_flush_region;
   ctx->base.transfer_unmap = lima_transfer_unmap;

   ctx->base.resource_copy_region = util_resource_copy_region;
   ctx->base.blit = lima_blit;

   ctx->base.flush_resource = lima_flush_resource;
}