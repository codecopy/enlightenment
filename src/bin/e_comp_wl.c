#include "e.h"
#include "e_comp_wl.h"
#include "e_comp_wl_input.h"
#include "e_comp_wl_data.h"
#include "e_surface.h"

#define E_COMP_WL_PIXMAP_CHECK \
   if (e_pixmap_type_get(ec->pixmap) != E_PIXMAP_TYPE_WL) return

/* local variables */
static Eina_List *handlers = NULL;
static Eina_Hash *clients_win_hash = NULL;
static Ecore_Idle_Enterer *_client_idler = NULL;
static Eina_List *_idle_clients = NULL;

static void
_e_comp_wl_client_event_free(void *d EINA_UNUSED, void *e)
{
   E_Event_Client *ev = e;

   e_object_unref(E_OBJECT(ev->ec));
   free(ev);
}

static void 
_e_comp_wl_buffer_pending_cb_destroy(struct wl_listener *listener, void *data EINA_UNUSED)
{
   E_Comp_Wl_Client_Data *cd;

   cd = container_of(listener, E_Comp_Wl_Client_Data, pending.buffer_destroy);
   cd->pending.buffer = NULL;
}

static void 
_e_comp_wl_buffer_cb_destroy(struct wl_listener *listener, void *data EINA_UNUSED)
{
   E_Comp_Wl_Buffer *buffer;

   buffer = container_of(listener, E_Comp_Wl_Buffer, destroy_listener);
   wl_signal_emit(&buffer->destroy_signal, buffer);
   free(buffer);
}

static E_Comp_Wl_Buffer *
_e_comp_wl_buffer_get(struct wl_resource *resource)
{
   E_Comp_Wl_Buffer *buffer;
   struct wl_listener *listener;

   listener = 
     wl_resource_get_destroy_listener(resource, _e_comp_wl_buffer_cb_destroy);

   if (listener)
     return container_of(listener, E_Comp_Wl_Buffer, destroy_listener);

   if (!(buffer = E_NEW(E_Comp_Wl_Buffer, 1))) return NULL;

   buffer->resource = resource;
   wl_signal_init(&buffer->destroy_signal);
   buffer->destroy_listener.notify = _e_comp_wl_buffer_cb_destroy;
   wl_resource_add_destroy_listener(resource, &buffer->destroy_listener);

   return buffer;
}

static void 
_e_comp_wl_buffer_reference_cb_destroy(struct wl_listener *listener, void *data EINA_UNUSED)
{
   E_Comp_Wl_Buffer_Ref *ref;

   if (!(ref = container_of(listener, E_Comp_Wl_Buffer_Ref, destroy_listener)))
     return;

   ref->buffer = NULL;
}

static void 
_e_comp_wl_buffer_reference(E_Comp_Wl_Buffer_Ref *ref, E_Comp_Wl_Buffer *buffer)
{
   if ((ref->buffer) && (buffer != ref->buffer))
     {
        ref->buffer->busy--;

        if (ref->buffer->busy == 0)
          wl_resource_queue_event(ref->buffer->resource, WL_BUFFER_RELEASE);

        wl_list_remove(&ref->destroy_listener.link);
     }

   if ((buffer) && (buffer != ref->buffer))
     {
        buffer->busy++;
        wl_signal_add(&buffer->destroy_signal, &ref->destroy_listener);
     }

   ref->buffer = buffer;
   ref->destroy_listener.notify = _e_comp_wl_buffer_reference_cb_destroy;
}

static void 
_e_comp_wl_surface_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void 
_e_comp_wl_surface_cb_attach(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer_resource, int32_t sx, int32_t sy)
{
   E_Pixmap *cp;
   E_Client *ec;
   E_Comp_Wl_Buffer *buffer;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;

   if (buffer_resource)
     {
        if (!(buffer = _e_comp_wl_buffer_get(buffer_resource)))
          {
             wl_client_post_no_memory(client);
             return;
          }
     }

   if (!ec->wl_comp_data) return;

   if (ec->wl_comp_data->pending.buffer)
     wl_list_remove(&ec->wl_comp_data->pending.buffer_destroy.link);

   ec->wl_comp_data->pending.x = sx;
   ec->wl_comp_data->pending.y = sy;
   ec->wl_comp_data->pending.w = 0;
   ec->wl_comp_data->pending.h = 0;
   ec->wl_comp_data->pending.buffer = buffer;
   ec->wl_comp_data->pending.new_attach = EINA_TRUE;

   if (buffer)
     {
        struct wl_shm_buffer *b;

        b = wl_shm_buffer_get(buffer_resource);
        ec->wl_comp_data->pending.w = wl_shm_buffer_get_width(b);
        ec->wl_comp_data->pending.h = wl_shm_buffer_get_height(b);

        wl_signal_add(&buffer->destroy_signal, 
                      &ec->wl_comp_data->pending.buffer_destroy);
     }
}

static void 
_e_comp_wl_surface_cb_damage(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
   E_Pixmap *cp;
   E_Client *ec;
   Eina_Tiler *tmp;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;
   if (!ec->wl_comp_data) return;

   tmp = eina_tiler_new(ec->w, ec->h);
   eina_tiler_tile_size_set(tmp, 1, 1);
   eina_tiler_rect_add(tmp, &(Eina_Rectangle){x, y, w, h});

   eina_tiler_union(ec->wl_comp_data->pending.damage, tmp);

   eina_tiler_free(tmp);
}

static void 
_e_comp_wl_surface_cb_frame_destroy(struct wl_resource *resource)
{
   E_Client *ec;

   if (!(ec = wl_resource_get_user_data(resource))) return;

   if (e_object_is_del(E_OBJECT(ec))) return;
   if (!ec->wl_comp_data) return;

   /* remove this frame callback from the list */
   ec->wl_comp_data->frames = 
     eina_list_remove(ec->wl_comp_data->frames, resource);
}

static void 
_e_comp_wl_surface_cb_frame(struct wl_client *client, struct wl_resource *resource, uint32_t callback)
{
   E_Pixmap *cp;
   E_Client *ec;
   struct wl_resource *res;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;
   if (!ec->wl_comp_data) return;

   /* create frame callback */
   res = wl_resource_create(client, &wl_callback_interface, 1, callback);
   if (!res)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   wl_resource_set_implementation(res, NULL, ec, 
                                  _e_comp_wl_surface_cb_frame_destroy);

   /* add this frame callback to the client */
   ec->wl_comp_data->frames = eina_list_prepend(ec->wl_comp_data->frames, res);
}

static void 
_e_comp_wl_surface_cb_opaque_region_set(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *region_resource)
{
   E_Pixmap *cp;
   E_Client *ec;

   /* DBG("E_Surface Opaque Region Set"); */

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;
   if (!ec->wl_comp_data) return;

   if (region_resource)
     {
        Eina_Tiler *tmp;

        if (!(tmp = wl_resource_get_user_data(region_resource)))
          return;

        eina_tiler_union(ec->wl_comp_data->pending.opaque, tmp);
     }
   else
     {
        eina_tiler_clear(ec->wl_comp_data->pending.opaque);
        eina_tiler_rect_add(ec->wl_comp_data->pending.opaque, 
                            &(Eina_Rectangle){0, 0, ec->client.w, ec->client.h});
     }
}

static void 
_e_comp_wl_surface_cb_input_region_set(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, struct wl_resource *region_resource)
{
   E_Pixmap *cp;
   E_Client *ec;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;
   if (!ec->wl_comp_data) return;

   if (region_resource)
     {
        Eina_Tiler *tmp;

        if (!(tmp = wl_resource_get_user_data(region_resource)))
          return;

        eina_tiler_union(ec->wl_comp_data->pending.input, tmp);
     }
   else
     {
        eina_tiler_clear(ec->wl_comp_data->pending.input);
        eina_tiler_rect_add(ec->wl_comp_data->pending.input, 
                            &(Eina_Rectangle){0, 0, ec->client.w, ec->client.h});
     }
}

static void 
_e_comp_wl_surface_cb_commit(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   E_Pixmap *cp;
   E_Client *ec;
   Eina_Tiler *src, *tmp;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if ((!ec) || (e_object_is_del(E_OBJECT(ec)))) return;

   if (!ec->wl_comp_data) return;

   if (ec->wl_comp_data->pending.new_attach)
     {
        _e_comp_wl_buffer_reference(&ec->wl_comp_data->buffer_ref,  
                                    ec->wl_comp_data->pending.buffer);

        e_pixmap_resource_set(cp, ec->wl_comp_data->pending.buffer->resource);
        e_pixmap_usable_set(cp, (ec->wl_comp_data->pending.buffer != NULL));
     }

   e_pixmap_dirty(cp);
   e_pixmap_refresh(cp);

   if ((ec->wl_comp_data->shell.surface) && 
       (ec->wl_comp_data->shell.configure))
     {
        if (ec->wl_comp_data->pending.new_attach)
          {
             if ((ec->client.w != ec->wl_comp_data->pending.w) || 
                 (ec->client.h != ec->wl_comp_data->pending.h))
               ec->wl_comp_data->shell.configure(ec->wl_comp_data->shell.surface, 
                                                 ec->client.x, ec->client.y, 
                                                 ec->wl_comp_data->pending.w, 
                                                 ec->wl_comp_data->pending.h);
          }
     }

   if (!ec->wl_comp_data->pending.buffer)
     {
        if (ec->wl_comp_data->mapped)
          {
             if ((ec->wl_comp_data->shell.surface) && 
                 (ec->wl_comp_data->shell.unmap))
               ec->wl_comp_data->shell.unmap(ec->wl_comp_data->shell.surface);
          }
     }
   else
     {
        if (!ec->wl_comp_data->mapped)
          {
             if ((ec->wl_comp_data->shell.surface) && 
                 (ec->wl_comp_data->shell.map))
               ec->wl_comp_data->shell.map(ec->wl_comp_data->shell.surface);
          }
     }

   /* reset pending buffer */
   if (ec->wl_comp_data->pending.buffer)
     wl_list_remove(&ec->wl_comp_data->pending.buffer_destroy.link);

   ec->wl_comp_data->pending.x = 0;
   ec->wl_comp_data->pending.y = 0;
   ec->wl_comp_data->pending.w = 0;
   ec->wl_comp_data->pending.h = 0;
   ec->wl_comp_data->pending.buffer = NULL;
   ec->wl_comp_data->pending.new_attach = EINA_FALSE;

   /* handle surface damages */
   if ((!ec->comp->nocomp) && (ec->frame))
     {
        tmp = eina_tiler_new(ec->w, ec->h);
        eina_tiler_tile_size_set(tmp, 1, 1);
        eina_tiler_rect_add(tmp,
                            &(Eina_Rectangle){0, 0, ec->client.w, ec->client.h});

        src = eina_tiler_intersection(ec->wl_comp_data->pending.damage, tmp);
        if (src)
          {
             Eina_Rectangle *rect;
             Eina_Iterator *itr;

             itr = eina_tiler_iterator_new(src);
             EINA_ITERATOR_FOREACH(itr, rect)
               {
                  e_comp_object_damage(ec->frame, 
                                       rect->x, rect->y, rect->w, rect->h);
               }
             eina_iterator_free(itr);
             eina_tiler_free(src);
          }

        eina_tiler_free(tmp);

        eina_tiler_clear(ec->wl_comp_data->pending.damage);
     }

   /* TODO !!! FIXME !!! */
   /* handle surface opaque region */

   tmp = eina_tiler_new(ec->w, ec->h);
   eina_tiler_tile_size_set(tmp, 1, 1);
   eina_tiler_rect_add(tmp, &(Eina_Rectangle){0, 0, ec->client.w, ec->client.h});

   src = eina_tiler_intersection(ec->wl_comp_data->pending.input, tmp);
   if (src)
     {
        Eina_Rectangle *rect;
        Eina_Iterator *itr;
        int i = 0;

        ec->shape_input_rects_num = 0;

        itr = eina_tiler_iterator_new(src);
        EINA_ITERATOR_FOREACH(itr, rect)
          {
             ec->shape_input_rects_num += 1;
          }

        ec->shape_input_rects = 
          malloc(sizeof(Eina_Rectangle) * ec->shape_input_rects_num);

        EINA_ITERATOR_FOREACH(itr, rect)
          {
             ec->shape_input_rects[i] = 
               *(Eina_Rectangle *)((char *)rect);

             ec->shape_input_rects[i].x = rect->x;
             ec->shape_input_rects[i].y = rect->y;
             ec->shape_input_rects[i].w = rect->w;
             ec->shape_input_rects[i].h = rect->h;

             i++;
          }

        eina_tiler_free(src);
     }

   eina_tiler_free(tmp);

   ec->changes.shape_input = EINA_TRUE;
   EC_CHANGED(ec);

/* #ifndef HAVE_WAYLAND_ONLY */
/*              e_comp_object_input_area_set(ec->frame, rect->x, rect->y,  */
/*                                           rect->w, rect->h); */
/* #endif */
}

static void 
_e_comp_wl_surface_cb_buffer_transform_set(struct wl_client *client EINA_UNUSED, struct wl_resource *resource EINA_UNUSED, int32_t transform EINA_UNUSED)
{
   DBG("Surface Buffer Transform");
}

static void 
_e_comp_wl_surface_cb_buffer_scale_set(struct wl_client *client EINA_UNUSED, struct wl_resource *resource EINA_UNUSED, int32_t scale EINA_UNUSED)
{
   DBG("Surface Buffer Scale");
}

static const struct wl_surface_interface _e_comp_wl_surface_interface = 
{
   _e_comp_wl_surface_cb_destroy,
   _e_comp_wl_surface_cb_attach,
   _e_comp_wl_surface_cb_damage,
   _e_comp_wl_surface_cb_frame,
   _e_comp_wl_surface_cb_opaque_region_set,
   _e_comp_wl_surface_cb_input_region_set,
   _e_comp_wl_surface_cb_commit,
   _e_comp_wl_surface_cb_buffer_transform_set,
   _e_comp_wl_surface_cb_buffer_scale_set
};

static void 
_e_comp_wl_comp_cb_surface_create(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
   E_Comp *comp;
   E_Pixmap *cp;
   struct wl_resource *res;
   uint64_t wid;
   pid_t pid;

   /* DBG("COMP_WL: Create Surface: %d", id); */

   if (!(comp = wl_resource_get_user_data(resource))) return;

   res = 
     e_comp_wl_surface_create(client, wl_resource_get_version(resource), id);
   if (!res)
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   /* get the client pid and generate a pixmap id */
   wl_client_get_credentials(client, &pid, NULL, NULL);
   wid = e_comp_wl_id_get(pid, id);

   /* see if we already have a pixmap for this surface */
   if (!(cp = e_pixmap_find(E_PIXMAP_TYPE_WL, wid)))
     {
        /* try to create a new pixmap for this surface */
        if (!(cp = e_pixmap_new(E_PIXMAP_TYPE_WL, wid)))
          {
             wl_resource_destroy(res);
             wl_resource_post_no_memory(resource);
             return;
          }
     }

   /* set reference to pixmap so we can fetch it later */
   wl_resource_set_user_data(res, cp);
}

static void 
_e_comp_wl_region_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void 
_e_comp_wl_region_cb_add(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
   Eina_Tiler *tiler;

   if ((tiler = wl_resource_get_user_data(resource)))
     {
        Eina_Tiler *src;

        src = eina_tiler_new(w, h);
        eina_tiler_tile_size_set(src, 1, 1);
        eina_tiler_rect_add(src, &(Eina_Rectangle){x, y, w, h});

        eina_tiler_union(tiler, src);
        eina_tiler_free(src);
     }
}

static void 
_e_comp_wl_region_cb_subtract(struct wl_client *client EINA_UNUSED, struct wl_resource *resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
   Eina_Tiler *tiler;

   if ((tiler = wl_resource_get_user_data(resource)))
     {
        Eina_Tiler *src;

        src = eina_tiler_new(w, h);
        eina_tiler_tile_size_set(src, 1, 1);
        eina_tiler_rect_add(src, &(Eina_Rectangle){x, y, w, h});

        eina_tiler_subtract(tiler, src);
        eina_tiler_free(src);
     }
}

static const struct wl_region_interface _e_region_interface = 
{
   _e_comp_wl_region_cb_destroy,
   _e_comp_wl_region_cb_add,
   _e_comp_wl_region_cb_subtract
};

static void 
_e_comp_wl_comp_cb_region_destroy(struct wl_resource *resource)
{
   Eina_Tiler *tiler;

   if ((tiler = wl_resource_get_user_data(resource)))
     eina_tiler_free(tiler);
}

static void 
_e_comp_wl_comp_cb_region_create(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
   E_Comp  *c;
   Eina_Tiler *tiler;
   struct wl_resource *res;

   if (!(c = e_comp_get(NULL)))
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   if (!(tiler = eina_tiler_new(c->man->w, c->man->h)))
     {
        wl_resource_post_no_memory(resource);
        return;
     }

   eina_tiler_tile_size_set(tiler, 1, 1);
   eina_tiler_rect_add(tiler, 
                       &(Eina_Rectangle){0, 0, c->man->w, c->man->h});

   if (!(res = wl_resource_create(client, &wl_region_interface, 1, id)))
     {
        eina_tiler_free(tiler);
        wl_resource_post_no_memory(resource);
        return;
     }

   wl_resource_set_implementation(res, &_e_region_interface, tiler, 
                                  _e_comp_wl_comp_cb_region_destroy);
}

static void 
_e_comp_wl_subcomp_cb_destroy(struct wl_client *client EINA_UNUSED, struct wl_resource *resource)
{
   wl_resource_destroy(resource);
}

static void 
_e_comp_wl_subcomp_cb_subsurface_get(struct wl_client *client EINA_UNUSED, struct wl_resource *resource EINA_UNUSED, uint32_t id EINA_UNUSED, struct wl_resource *surface_resource EINA_UNUSED, struct wl_resource *parent_resource EINA_UNUSED)
{
   /* NB: Needs New Resource */
#warning TODO Need to subcomp subsurface
}

static const struct wl_compositor_interface _e_comp_interface = 
{
   _e_comp_wl_comp_cb_surface_create,
   _e_comp_wl_comp_cb_region_create
};

static const struct wl_subcompositor_interface _e_subcomp_interface = 
{
   _e_comp_wl_subcomp_cb_destroy,
   _e_comp_wl_subcomp_cb_subsurface_get
};

static void 
_e_comp_wl_cb_bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   E_Comp *comp;
   struct wl_resource *res;

   if (!(comp = data)) return;

   res = 
     wl_resource_create(client, &wl_compositor_interface, 
                        MIN(version, 3), id);
   if (!res)
     {
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_comp_interface, comp, NULL);
}

static void 
_e_comp_wl_cb_bind_subcompositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   E_Comp *comp;
   struct wl_resource *res;

   if (!(comp = data)) return;

   res = 
     wl_resource_create(client, &wl_subcompositor_interface, 
                        MIN(version, 1), id);
   if (!res)
     {
        wl_client_post_no_memory(client);
        return;
     }

   wl_resource_set_implementation(res, &_e_subcomp_interface, comp, NULL);
}

static void 
_e_comp_wl_cb_render_post(void *data EINA_UNUSED, Evas *evas EINA_UNUSED, void *event EINA_UNUSED)
{
   Eina_Iterator *itr;
   E_Client *ec;

   if (!(itr = eina_hash_iterator_data_new(clients_win_hash))) return;

   EINA_ITERATOR_FOREACH(itr, ec)
     {
        struct wl_resource *cb;

        if (!ec->wl_comp_data) continue;
        EINA_LIST_FREE(ec->wl_comp_data->frames, cb)
          {
             wl_callback_send_done(cb, ecore_loop_time_get());
             wl_resource_destroy(cb);
          }

        /* post a buffer release */
        /* TODO: FIXME: We need a way to determine if the client wants to 
         * keep the buffer or not. If so, then we should Not be setting NULL 
         * here as this will essentially release the buffer */
        _e_comp_wl_buffer_reference(&ec->wl_comp_data->buffer_ref, NULL);
     }

   eina_iterator_free(itr);
}

static void 
_e_comp_wl_cb_del(E_Comp *comp)
{
   E_Comp_Wl_Data *cdata;

   cdata = comp->wl_comp_data;

   e_comp_wl_data_manager_shutdown(cdata);
   e_comp_wl_input_shutdown(cdata);

   /* remove render_post callback */
   evas_event_callback_del_full(comp->evas, EVAS_CALLBACK_RENDER_POST, 
                                _e_comp_wl_cb_render_post, NULL);

   /* delete idler to flush clients */
   if (cdata->idler) ecore_idler_del(cdata->idler);

   /* delete fd handler to listen for wayland main loop events */
   if (cdata->fd_hdlr) ecore_main_fd_handler_del(cdata->fd_hdlr);

   /* delete the wayland display */
   if (cdata->wl.disp) wl_display_destroy(cdata->wl.disp);

   free(cdata);
}

static Eina_Bool 
_e_comp_wl_cb_read(void *data, Ecore_Fd_Handler *hdl EINA_UNUSED)
{
   E_Comp_Wl_Data *cdata;

   if (!(cdata = data)) return ECORE_CALLBACK_RENEW;
   if (!cdata->wl.disp) return ECORE_CALLBACK_RENEW;

   /* dispatch any pending main loop events */
   wl_event_loop_dispatch(cdata->wl.loop, 0);

   /* flush any pending client events */
   wl_display_flush_clients(cdata->wl.disp);

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool 
_e_comp_wl_cb_idle(void *data)
{
   E_Comp_Wl_Data *cdata;

   if (!(cdata = data)) return ECORE_CALLBACK_RENEW;
   if (!cdata->wl.disp) return ECORE_CALLBACK_RENEW;

   /* flush any pending client events */
   wl_display_flush_clients(cdata->wl.disp);

   /* dispatch any pending main loop events */
   wl_event_loop_dispatch(cdata->wl.loop, 0);

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool 
_e_comp_wl_cb_module_idle(void *data)
{
   E_Module *mod = NULL;
   E_Comp_Wl_Data *cdata;

   if (!(cdata = data)) return ECORE_CALLBACK_RENEW;

   if (e_module_loading_get()) return ECORE_CALLBACK_RENEW;

   /* FIXME: make which shell to load configurable */
   if (!(mod = e_module_find("wl_desktop_shell")))
     mod = e_module_new("wl_desktop_shell");

   if (mod)
     {
        e_module_enable(mod);

        /* dispatch any pending main loop events */
        wl_event_loop_dispatch(cdata->wl.loop, 0);

        return ECORE_CALLBACK_CANCEL;
     }

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool 
_e_comp_wl_cb_first_draw(void *data)
{
   E_Client *ec;

   if (!(ec = data)) return EINA_TRUE;
   ec->wl_comp_data->first_draw_tmr = NULL;
   e_comp_object_damage(ec->frame, 0, 0, ec->w, ec->h);
   return EINA_FALSE;
}

static Eina_Bool 
_e_comp_wl_compositor_create(void)
{
   E_Comp *comp;
   E_Comp_Wl_Data *cdata;
   char buff[PATH_MAX];
   /* char *rules, *model, *layout; */
   int fd = 0;

   /* get the current compositor */
   if (!(comp = e_comp_get(NULL)))
     {
        comp = e_comp_new();
        comp->comp_type = E_PIXMAP_TYPE_WL;
        E_OBJECT_DEL_SET(comp, _e_comp_wl_cb_del);
     }

   /* check compositor type and make sure it's Wayland */
   /* if (comp->comp_type != E_PIXMAP_TYPE_WL) return EINA_FALSE; */

   cdata = E_NEW(E_Comp_Wl_Data, 1);
   comp->wl_comp_data = cdata;

   /* setup wayland display environment variable */
   snprintf(buff, sizeof(buff), "%s/wayland-0", e_ipc_socket);
   e_env_set("WAYLAND_DISPLAY", buff);

   /* try to create wayland display */
   if (!(cdata->wl.disp = wl_display_create()))
     {
        ERR("Could not create a Wayland Display: %m");
        goto disp_err;
     }

   if (wl_display_add_socket(cdata->wl.disp, buff))
     {
        ERR("Could not create a Wayland Display: %m");
        goto disp_err;
     }

   /* setup wayland compositor signals */
   /* NB: So far, we don't need ANY of these... */
   /* wl_signal_init(&cdata->signals.destroy); */
   /* wl_signal_init(&cdata->signals.activate); */
   /* wl_signal_init(&cdata->signals.transform); */
   /* wl_signal_init(&cdata->signals.kill); */
   /* wl_signal_init(&cdata->signals.idle); */
   /* wl_signal_init(&cdata->signals.wake); */
   /* wl_signal_init(&cdata->signals.session); */
   /* wl_signal_init(&cdata->signals.seat.created); */
   /* wl_signal_init(&cdata->signals.seat.destroyed); */
   /* wl_signal_init(&cdata->signals.seat.moved); */
   /* wl_signal_init(&cdata->signals.output.created); */
   /* wl_signal_init(&cdata->signals.output.destroyed); */
   /* wl_signal_init(&cdata->signals.output.moved); */

   /* try to add compositor to wayland display globals */
   if (!wl_global_create(cdata->wl.disp, &wl_compositor_interface, 3, 
                         comp, _e_comp_wl_cb_bind_compositor))
     {
        ERR("Could not add compositor to globals: %m");
        goto disp_err;
     }

   /* try to add subcompositor to wayland display globals */
   if (!wl_global_create(cdata->wl.disp, &wl_subcompositor_interface, 1, 
                         comp, _e_comp_wl_cb_bind_subcompositor))
     {
        ERR("Could not add compositor to globals: %m");
        goto disp_err;
     }

   /* try to init data manager */
   if (!e_comp_wl_data_manager_init(cdata))
     {
        ERR("Could not initialize data manager");
        goto disp_err;
     }

   /* try to init input (keyboard & pointer) */
   if (!e_comp_wl_input_init(cdata))
     {
        ERR("Could not initialize input");
        goto disp_err;
     }

/* #ifndef HAVE_WAYLAND_ONLY */
/*    if (getenv("DISPLAY")) */
/*      { */
/*         E_Config_XKB_Layout *ekbd; */
/*         Ecore_X_Atom xkb = 0; */
/*         Ecore_X_Window root = 0; */
/*         int len = 0; */
/*         unsigned char *dat; */

/*         if ((ekbd = e_xkb_layout_get())) */
/*           { */
/*              model = strdup(ekbd->model); */
/*              layout = strdup(ekbd->name); */
/*           } */

/*         root = ecore_x_window_root_first_get(); */
/*         xkb = ecore_x_atom_get("_XKB_RULES_NAMES"); */
/*         ecore_x_window_prop_property_get(root, xkb, ECORE_X_ATOM_STRING,  */
/*                                          1024, &dat, &len); */
/*         if ((dat) && (len > 0)) */
/*           { */
/*              rules = (char *)dat; */
/*              dat += strlen((const char *)dat) + 1; */
/*              if (!model) model = strdup((const char *)dat); */
/*              dat += strlen((const char *)dat) + 1; */
/*              if (!layout) layout = strdup((const char *)dat); */
/*           } */
/*      } */
/* #endif */

   /* fallback */
   /* if (!rules) rules = strdup("evdev"); */
   /* if (!model) model = strdup("pc105"); */
   /* if (!layout) layout = strdup("us"); */

   /* update compositor keymap */
   /* e_comp_wl_input_keymap_set(cdata, rules, model, layout); */

   /* TODO: init text backend */

   /* initialize shm mechanism */
   wl_display_init_shm(cdata->wl.disp);

   /* check for gl rendering */
   if ((e_comp_gl_get()) && 
       (e_comp_config_get()->engine == E_COMP_ENGINE_GL))
     {
        /* TODO: setup gl ? */
     }

   /* get the wayland display's event loop */
   cdata->wl.loop= wl_display_get_event_loop(cdata->wl.disp);

   /* get the file descriptor of the main loop */
   fd = wl_event_loop_get_fd(cdata->wl.loop);

   /* add an fd handler to listen for wayland main loop events */
   cdata->fd_hdlr = 
     ecore_main_fd_handler_add(fd, ECORE_FD_READ | ECORE_FD_WRITE, 
                               _e_comp_wl_cb_read, cdata, NULL, NULL);

   /* add an idler to flush clients */
   cdata->idler = ecore_idle_enterer_add(_e_comp_wl_cb_idle, cdata);

   /* setup module idler to load shell module */
   ecore_idler_add(_e_comp_wl_cb_module_idle, cdata);

   /* add a render post callback so we can send frame_done to the surface */
   evas_event_callback_add(comp->evas, EVAS_CALLBACK_RENDER_POST, 
                           _e_comp_wl_cb_render_post, NULL);

   return EINA_TRUE;

disp_err:
   e_env_unset("WAYLAND_DISPLAY");
   return EINA_FALSE;
}

static Eina_Bool 
_e_comp_wl_client_idler(void *data EINA_UNUSED)
{
   E_Client *ec;
   E_Comp *comp;
   const Eina_List *l;

   EINA_LIST_FREE(_idle_clients, ec)
     {
        if ((e_object_is_del(E_OBJECT(ec))) || (!ec->wl_comp_data)) continue;

        ec->post_move = 0;
        ec->post_resize = 0;
     }

   EINA_LIST_FOREACH(e_comp_list(), l, comp)
     {
        if ((comp->wl_comp_data->restack) && (!comp->new_clients))
          {
             e_hints_client_stacking_set();
             comp->wl_comp_data->restack = EINA_FALSE;
          }
     }

   _client_idler = NULL;
   return EINA_FALSE;
}

static void 
_e_comp_wl_client_idler_add(E_Client *ec)
{
   if (!_client_idler) 
     _client_idler = ecore_idle_enterer_add(_e_comp_wl_client_idler, NULL);

   if (!ec) CRI("ACK!");

   if (!eina_list_data_find(_idle_clients, ec))
     _idle_clients = eina_list_append(_idle_clients, ec);
}

static void 
_e_comp_wl_evas_cb_show(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec, *tmp;
   Eina_List *l;

   if (!(ec = data)) return;

   if (!ec->override) 
     e_hints_window_visible_set(ec);

   if (ec->wl_comp_data->frame_update)
     ec->wl_comp_data->frame_update = EINA_FALSE;

   EINA_LIST_FOREACH(ec->e.state.video_child, l, tmp)
     evas_object_show(tmp->frame);
}

static void 
_e_comp_wl_evas_cb_hide(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec, *tmp;
   Eina_List *l;

   if (!(ec = data)) return;

   EINA_LIST_FOREACH(ec->e.state.video_child, l, tmp)
     evas_object_hide(tmp->frame);
}

static void 
_e_comp_wl_evas_cb_mouse_in(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   Evas_Event_Mouse_In *ev;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial;

   ev = event;
   if (!(ec = data)) return;
   if (!ec->wl_comp_data) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->ptr.resources, l, res)
     {
        if (!e_comp_wl_input_pointer_check(res)) continue;
        if (wl_resource_get_client(res) != wc) continue;
        wl_pointer_send_enter(res, serial, ec->wl_comp_data->surface, 
                              wl_fixed_from_int(ev->canvas.x), 
                              wl_fixed_from_int(ev->canvas.y));
     }
}

static void 
_e_comp_wl_evas_cb_mouse_out(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial;

   if (!(ec = data)) return;
   if (!ec->wl_comp_data) return;
   if (ec->cur_mouse_action) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->ptr.resources, l, res)
     {
        if (!e_comp_wl_input_pointer_check(res)) continue;
        if (wl_resource_get_client(res) != wc) continue;
        wl_pointer_send_leave(res, serial, ec->wl_comp_data->surface);
     }
}

static void 
_e_comp_wl_evas_cb_mouse_down(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   Evas_Event_Mouse_Down *ev;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial, btn;

   ev = event;
   if (!(ec = data)) return;
   if (ec->cur_mouse_action) return;
   if (e_object_is_del(E_OBJECT(ec))) return;
   if (e_client_util_ignored_get(ec)) return;

   switch (ev->button)
     {
      case 1:
        btn = BTN_LEFT;
        break;
      case 2:
        btn = BTN_MIDDLE;
        break;
      case 3:
        btn = BTN_RIGHT;
        break;
      default:
        btn = ev->button;
        break;
     }

   ec->comp->wl_comp_data->ptr.button = btn;

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->ptr.resources, l, res)
     {
        if (!e_comp_wl_input_pointer_check(res)) continue;
        if (wl_resource_get_client(res) != wc) continue;
        wl_pointer_send_button(res, serial, ev->timestamp, btn, 
                               WL_POINTER_BUTTON_STATE_PRESSED);
     }
}

static void 
_e_comp_wl_evas_cb_mouse_up(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   Evas_Event_Mouse_Up *ev;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial, btn;

   ev = event;
   if (!(ec = data)) return;
   if (ec->cur_mouse_action) return;
   if (e_object_is_del(E_OBJECT(ec))) return;
   if (e_client_util_ignored_get(ec)) return;

   switch (ev->button)
     {
      case 1:
        btn = BTN_LEFT;
        break;
      case 2:
        btn = BTN_MIDDLE;
        break;
      case 3:
        btn = BTN_RIGHT;
        break;
      default:
        btn = ev->button;
        break;
     }

   ec->comp->wl_comp_data->resize.resource = NULL;
   ec->comp->wl_comp_data->ptr.button = btn;

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->ptr.resources, l, res)
     {
        if (!e_comp_wl_input_pointer_check(res)) continue;
        if (wl_resource_get_client(res) != wc) continue;
        wl_pointer_send_button(res, serial, ev->timestamp, btn, 
                               WL_POINTER_BUTTON_STATE_RELEASED);
     }
}

static void 
_e_comp_wl_evas_cb_mouse_move(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   Evas_Event_Mouse_Move *ev;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;

   ev = event;
   if (!(ec = data)) return;
   if (ec->cur_mouse_action) return;
   if (e_object_is_del(E_OBJECT(ec))) return;
   if (e_client_util_ignored_get(ec)) return;

   ec->comp->wl_comp_data->ptr.x = 
     wl_fixed_from_int(ev->cur.canvas.x - ec->client.x);
   ec->comp->wl_comp_data->ptr.y = 
     wl_fixed_from_int(ev->cur.canvas.y - ec->client.y);

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->ptr.resources, l, res)
     {
        if (!e_comp_wl_input_pointer_check(res)) continue;
        if (wl_resource_get_client(res) != wc) continue;
        wl_pointer_send_motion(res, ev->timestamp, 
                               ec->comp->wl_comp_data->ptr.x, 
                               ec->comp->wl_comp_data->ptr.y);
     }
}

static void 
_e_comp_wl_evas_cb_key_down(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   E_Comp_Wl_Data *cdata;
   Evas_Event_Key_Down *ev;
   uint32_t serial, *end, *k, keycode;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;

   ev = event;
   if (!(ec = data)) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   if (!ec->focused) return;

   keycode = (ev->keycode - 8);
   cdata = ec->comp->wl_comp_data;

   end = (uint32_t *)cdata->kbd.keys.data + cdata->kbd.keys.size;

   for (k = cdata->kbd.keys.data; k < end; k++)
     {
        /* ignore server-generated key repeats */
        if (*k == keycode) return;
        *k = *--end;
     }

   cdata->kbd.keys.size = end - (uint32_t *)cdata->kbd.keys.data;
   k = wl_array_add(&cdata->kbd.keys, sizeof(*k));
   *k = keycode;

   /* update modifier state */
   e_comp_wl_input_keyboard_state_update(cdata, keycode, EINA_TRUE);

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->kbd.resources, l, res)
     {
        if (wl_resource_get_client(res) != wc) continue;
        wl_keyboard_send_key(res, serial, ev->timestamp, 
                             keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
     }
}

static void 
_e_comp_wl_evas_cb_key_up(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   E_Client *ec;
   E_Comp_Wl_Data *cdata;
   Evas_Event_Key_Up *ev;
   uint32_t serial, *end, *k, keycode;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;

   ev = event;
   if (!(ec = data)) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   if (!ec->focused) return;

   keycode = (ev->keycode - 8);
   cdata = ec->comp->wl_comp_data;

   end = (uint32_t *)cdata->kbd.keys.data + cdata->kbd.keys.size;
   for (k = cdata->kbd.keys.data; k < end; k++)
     if (*k == keycode) *k = *--end;

   cdata->kbd.keys.size = end - (uint32_t *)cdata->kbd.keys.data;

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(cdata->wl.disp);
   EINA_LIST_FOREACH(cdata->kbd.resources, l, res)
     {
        if (wl_resource_get_client(res) != wc) continue;
        wl_keyboard_send_key(res, serial, ev->timestamp, 
                             keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
     }

   /* update modifier state */
   e_comp_wl_input_keyboard_state_update(cdata, keycode, EINA_FALSE);
}

static void 
_e_comp_wl_evas_cb_focus_in(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec, *focused;
   E_Comp_Wl_Data *cdata;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial, *k;

   if (!(ec = data)) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   /* block refocus attempts on iconic clients
    * these result from iconifying a client during a grab */
   if (ec->iconic) return;

   /* block spurious focus events
    * not sure if correct, but seems necessary to use pointer focus... */
   focused = e_client_focused_get();
   if (focused && (ec != focused)) return;

   cdata = ec->comp->wl_comp_data;

   /* TODO: priority raise */

   /* update modifier state */
   wl_array_for_each(k, &cdata->kbd.keys)
     e_comp_wl_input_keyboard_state_update(cdata, *k, EINA_TRUE);

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(cdata->wl.disp);
   EINA_LIST_FOREACH(cdata->kbd.resources, l, res)
     {
        if (wl_resource_get_client(res) != wc) continue;
        wl_keyboard_send_enter(res, serial, ec->wl_comp_data->surface, 
                               &cdata->kbd.keys);
     }
}

static void 
_e_comp_wl_evas_cb_focus_out(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;
   struct wl_resource *res;
   struct wl_client *wc;
   Eina_List *l;
   uint32_t serial, *k;

   if (!(ec = data)) return;
   if (e_object_is_del(E_OBJECT(ec))) return;

   /* TODO: priority normal */

   /* update modifier state */
   wl_array_for_each(k, &ec->comp->wl_comp_data->kbd.keys)
     e_comp_wl_input_keyboard_state_update(ec->comp->wl_comp_data, *k, EINA_FALSE);

   wc = wl_resource_get_client(ec->wl_comp_data->surface);
   serial = wl_display_next_serial(ec->comp->wl_comp_data->wl.disp);
   EINA_LIST_FOREACH(ec->comp->wl_comp_data->kbd.resources, l, res)
     {
        if (wl_resource_get_client(res) != wc) continue;
        wl_keyboard_send_leave(res, serial, ec->wl_comp_data->surface);
     }
}

static void 
_e_comp_wl_evas_cb_resize(void *data, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;
   E_Comp_Wl_Data *cdata;

   if (!(ec = data)) return;
   if ((ec->shading) || (ec->shaded)) return;
   if (!e_pixmap_size_changed(ec->pixmap, ec->client.w, ec->client.h))
     return;

   /* DBG("COMP_WL: Evas Resize: %d %d", ec->client.w, ec->client.h); */

   cdata = ec->comp->wl_comp_data;

   /* if ((ec->changes.pos) || (ec->changes.size)) */
     {
        if ((ec->wl_comp_data) && (ec->wl_comp_data->shell.configure_send))
          ec->wl_comp_data->shell.configure_send(ec->wl_comp_data->shell.surface, 
                                                 cdata->resize.edges,
                                                 ec->client.w, ec->client.h);
     }

   ec->post_resize = EINA_TRUE;
   /* e_pixmap_dirty(ec->pixmap); */
   /* e_comp_object_render_update_del(ec->frame); */
   _e_comp_wl_client_idler_add(ec);
}

static void 
_e_comp_wl_evas_cb_frame_recalc(void *data, Evas_Object *obj, void *event)
{
   E_Client *ec;
   E_Comp_Object_Frame *fr;

   fr = event;
   if (!(ec = data)) return;
   if (!ec->wl_comp_data) return;
   WRN("COMP_WL Frame Recalc: %d %d %d %d", fr->l, fr->r, fr->t, fr->b);
   if (evas_object_visible_get(obj))
     ec->wl_comp_data->frame_update = EINA_FALSE;
   else
     ec->wl_comp_data->frame_update = EINA_TRUE;
   ec->post_move = ec->post_resize = EINA_TRUE;
   _e_comp_wl_client_idler_add(ec);
}

/* static void  */
/* _e_comp_wl_evas_cb_comp_hidden(void *data, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED) */
/* { */
/*    E_Client *ec; */

/*    if (!(ec = data)) return; */
/* #warning FIXME Implement Evas Comp Hidden */
/* } */

static void 
_e_comp_wl_evas_cb_delete_request(void *data, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;

   DBG("COMP_WL: Evas Del Request");

   if (!(ec = data)) return;
   if (ec->netwm.ping) e_client_ping(ec);

   /* if (ec->wl_comp_data->shell.surface) */
   /*   wl_resource_destroy(ec->wl_comp_data->shell.surface); */


   /* FIXME !!!
    * 
    * This is a HUGE problem for internal windows...
    * 
    * IF we delete the client here, then we cannot reopen some internal 
    * dialogs (configure, etc, etc) ...
    * 
    * BUT, if we don't handle delete_request Somehow, then the close button on 
    * the frame does Nothing
    * 
    */


   /* e_comp_ignore_win_del(E_PIXMAP_TYPE_WL, e_pixmap_window_get(ec->pixmap)); */
   /* if (ec->wl_comp_data) */
   /*   { */
   /*      if (ec->wl_comp_data->reparented) */
   /*        e_client_comp_hidden_set(ec, EINA_TRUE); */
   /*   } */

   /* evas_object_pass_events_set(ec->frame, EINA_TRUE); */
   /* if (ec->visible) evas_object_hide(ec->frame); */
   /* e_object_del(E_OBJECT(ec)); */

   /* TODO: Delete request send ?? */
#warning TODO Need to implement delete request ?
}

static void 
_e_comp_wl_evas_cb_kill_request(void *data, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;

   if (!(ec = data)) return;
   if (ec->netwm.ping) e_client_ping(ec);

   e_comp_ignore_win_del(E_PIXMAP_TYPE_WL, e_pixmap_window_get(ec->pixmap));
   if (ec->wl_comp_data)
     {
        if (ec->wl_comp_data->reparented)
          e_client_comp_hidden_set(ec, EINA_TRUE);
        evas_object_pass_events_set(ec->frame, EINA_TRUE);
        evas_object_hide(ec->frame);
        e_object_del(E_OBJECT(ec));
     }
}

static void 
_e_comp_wl_evas_cb_ping(void *data, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   E_Client *ec;

   if (!(ec = data)) return;
   if (!ec->wl_comp_data) return;

   if (ec->wl_comp_data->shell.ping)
     {
        if (ec->wl_comp_data->shell.surface)
          ec->wl_comp_data->shell.ping(ec->wl_comp_data->shell.surface);
     }
}

static void 
_e_comp_wl_evas_cb_color_set(void *data, Evas_Object *obj, void *event EINA_UNUSED)
{
   E_Client *ec;
   int a = 0;

   if (!(ec = data)) return;
   if (!ec->wl_comp_data) return;
   evas_object_color_get(obj, NULL, NULL, NULL, &a);
   if (ec->netwm.opacity == a) return;
   ec->netwm.opacity = a;
   ec->netwm.opacity_changed = EINA_TRUE;
   _e_comp_wl_client_idler_add(ec);
}

static void 
_e_comp_wl_client_evas_init(E_Client *ec)
{
   if (!ec->wl_comp_data) return;
   if (ec->wl_comp_data->evas_init) return;
   ec->wl_comp_data->evas_init = EINA_TRUE;

   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_SHOW, 
                                  _e_comp_wl_evas_cb_show, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_HIDE, 
                                  _e_comp_wl_evas_cb_hide, ec);

   /* we need to hook evas mouse events for wayland clients */
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOUSE_IN, 
                                  _e_comp_wl_evas_cb_mouse_in, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOUSE_OUT, 
                                  _e_comp_wl_evas_cb_mouse_out, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOUSE_DOWN, 
                                  _e_comp_wl_evas_cb_mouse_down, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOUSE_UP, 
                                  _e_comp_wl_evas_cb_mouse_up, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_MOUSE_MOVE, 
                                  _e_comp_wl_evas_cb_mouse_move, ec);

   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_KEY_DOWN, 
                                  _e_comp_wl_evas_cb_key_down, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_KEY_UP, 
                                  _e_comp_wl_evas_cb_key_up, ec);

   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_FOCUS_IN, 
                                  _e_comp_wl_evas_cb_focus_in, ec);
   evas_object_event_callback_add(ec->frame, EVAS_CALLBACK_FOCUS_OUT, 
                                  _e_comp_wl_evas_cb_focus_out, ec);

   if (!ec->override)
     {
        evas_object_smart_callback_add(ec->frame, "client_resize", 
                                       _e_comp_wl_evas_cb_resize, ec);
     }

   evas_object_smart_callback_add(ec->frame, "frame_recalc_done", 
                                  _e_comp_wl_evas_cb_frame_recalc, ec);
   evas_object_smart_callback_add(ec->frame, "delete_request", 
                                  _e_comp_wl_evas_cb_delete_request, ec);
   evas_object_smart_callback_add(ec->frame, "kill_request", 
                                  _e_comp_wl_evas_cb_kill_request, ec);
   evas_object_smart_callback_add(ec->frame, "ping", 
                                  _e_comp_wl_evas_cb_ping, ec);
   evas_object_smart_callback_add(ec->frame, "color_set", 
                                  _e_comp_wl_evas_cb_color_set, ec);

   /* TODO: these will need to send_configure */
   /* evas_object_smart_callback_add(ec->frame, "fullscreen_zoom",  */
   /*                                _e_comp_wl_evas_cb_resize, ec); */
   /* evas_object_smart_callback_add(ec->frame, "unfullscreen_zoom",  */
   /*                                _e_comp_wl_evas_cb_resize, ec); */
}

static Eina_Bool 
_e_comp_wl_client_new_helper(E_Client *ec)
{
   /* FIXME */
   ec->border_size = 0;

   ec->placed |= ec->override;
   ec->icccm.accepts_focus = ((!ec->override) && (!ec->input_only));

   if ((ec->override) && ((ec->x == -77) && (ec->y == -777)))
     {
        e_comp_ignore_win_add(E_PIXMAP_TYPE_WL, e_client_util_win_get(ec));
        e_object_del(E_OBJECT(ec));
        return EINA_FALSE;
     }

   /* if ((!e_client_util_ignored_get(ec)) &&  */
   /*     (!ec->internal) && (!ec->internal_ecore_evas)) */
   /*   { */
   /*      ec->wl_comp_data->need_reparent = EINA_TRUE; */
   /*      EC_CHANGED(ec); */
   /*      ec->take_focus = !starting; */
   /*   } */
   ec->new_client ^= ec->override;

   if (e_pixmap_size_changed(ec->pixmap, ec->client.w, ec->client.h))
     {
        ec->changes.size = EINA_TRUE;
        EC_CHANGED(ec);
     }

   return EINA_TRUE;
}

static Eina_Bool 
_e_comp_wl_client_shape_check(E_Client *ec)
{
   /* FIXME: need way to determine if shape has changed */
   ec->shape_changed = EINA_TRUE;
   e_comp_shape_queue(ec->comp);
   return EINA_TRUE;
}

static Eina_Bool 
_e_comp_wl_cb_comp_object_add(void *data EINA_UNUSED, int type EINA_UNUSED, E_Event_Comp_Object *ev)
{
   E_Client *ec;

   ec = e_comp_object_client_get(ev->comp_object);

   /* NB: Don't check re_manage here as we need evas events for mouse */
   if ((!ec) || (e_object_is_del(E_OBJECT(ec))))
     return ECORE_CALLBACK_RENEW;

   E_COMP_WL_PIXMAP_CHECK ECORE_CALLBACK_RENEW;

   _e_comp_wl_client_evas_init(ec);

   return ECORE_CALLBACK_RENEW;
}

/* static Eina_Bool  */
/* _e_comp_wl_cb_client_zone_set(void *data EINA_UNUSED, int type EINA_UNUSED, void *event) */
/* { */
/*    E_Event_Client *ev; */
/*    E_Client *ec; */

/*    DBG("CLIENT ZONE SET !!!"); */

/*    ev = event; */
/*    if (!(ec = ev->ec)) return ECORE_CALLBACK_RENEW; */
/*    if (e_object_is_del(E_OBJECT(ec))) return ECORE_CALLBACK_RENEW; */
/*    E_COMP_WL_PIXMAP_CHECK ECORE_CALLBACK_RENEW; */

/*    DBG("\tClient Zone: %d", (ec->zone != NULL)); */

/*    return ECORE_CALLBACK_RENEW; */
/* } */

static Eina_Bool 
_e_comp_wl_cb_client_prop(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   E_Client *ec;
   E_Event_Client_Property *ev;

   ev = event;
   if (!(ev->property & E_CLIENT_PROPERTY_ICON)) return ECORE_CALLBACK_RENEW;

   ec = ev->ec;
   E_COMP_WL_PIXMAP_CHECK ECORE_CALLBACK_RENEW;

   if (ec->desktop)
     {
        if (!ec->exe_inst) e_exec_phony(ec);
     }

   return ECORE_CALLBACK_RENEW;
}

static void 
_e_comp_wl_cb_hook_client_move_end(void *data EINA_UNUSED, E_Client *ec)
{
   E_COMP_WL_PIXMAP_CHECK;

   /* DBG("COMP_WL HOOK CLIENT MOVE END !!"); */

   /* unset pointer */
   /* e_pointer_type_pop(e_comp_get(ec)->pointer, ec, "move"); */

   /* ec->post_move = EINA_TRUE; */
   /* _e_comp_wl_client_idler_add(ec); */
}

static void 
_e_comp_wl_cb_hook_client_del(void *data EINA_UNUSED, E_Client *ec)
{
   uint64_t win;

   E_COMP_WL_PIXMAP_CHECK;

   if ((!ec->already_unparented) && (ec->wl_comp_data->reparented))
     {
        /* TODO: focus setdown */
#warning TODO Need to implement focus setdown
     }

   ec->already_unparented = EINA_TRUE;
   win = e_pixmap_window_get(ec->pixmap);
   eina_hash_del_by_key(clients_win_hash, &win);

   if (ec->wl_comp_data->pending.damage)
     eina_tiler_free(ec->wl_comp_data->pending.damage);
   if (ec->wl_comp_data->pending.input)
     eina_tiler_free(ec->wl_comp_data->pending.input);
   if (ec->wl_comp_data->pending.opaque)
     eina_tiler_free(ec->wl_comp_data->pending.opaque);

   if (ec->wl_comp_data->reparented)
     {
        win = e_client_util_pwin_get(ec);
        eina_hash_del_by_key(clients_win_hash, &win);
        e_pixmap_parent_window_set(ec->pixmap, 0);
     }

   if ((ec->parent) && (ec->parent->modal == ec))
     {
        ec->parent->lock_close = EINA_FALSE;
        ec->parent->modal = NULL;
     }

   E_FREE_FUNC(ec->wl_comp_data->first_draw_tmr, ecore_timer_del);

   E_FREE(ec->wl_comp_data);
   ec->wl_comp_data = NULL;

   /* TODO: comp focus check */
}

static void 
_e_comp_wl_cb_hook_client_new(void *data EINA_UNUSED, E_Client *ec)
{
   uint64_t win;

   E_COMP_WL_PIXMAP_CHECK;

   win = e_pixmap_window_get(ec->pixmap);
   ec->ignored = e_comp_ignore_win_find(win);

   /* NB: could not find a better place todo this, BUT for internal windows, 
    * we need to set delete_request else the close buttons on the frames do 
    * basically nothing */
   if (ec->internal) ec->icccm.delete_request = EINA_TRUE;

   ec->wl_comp_data = E_NEW(E_Comp_Wl_Client_Data, 1);

   ec->wl_comp_data->pending.damage = eina_tiler_new(ec->w, ec->h);
   eina_tiler_tile_size_set(ec->wl_comp_data->pending.damage, 1, 1);

   ec->wl_comp_data->pending.input = eina_tiler_new(ec->w, ec->h);
   eina_tiler_tile_size_set(ec->wl_comp_data->pending.input, 1, 1);

   ec->wl_comp_data->pending.opaque = eina_tiler_new(ec->w, ec->h);
   eina_tiler_tile_size_set(ec->wl_comp_data->pending.opaque, 1, 1);

   ec->wl_comp_data->pending.buffer_destroy.notify = 
     _e_comp_wl_buffer_pending_cb_destroy;

   ec->wl_comp_data->mapped = EINA_FALSE;
   ec->wl_comp_data->set_win_type = EINA_TRUE;
   ec->netwm.type = E_WINDOW_TYPE_UNKNOWN;
   /* ec->shaped = EINA_TRUE; */
   /* ec->shaped_input = EINA_TRUE; */
   ec->changes.shape = EINA_TRUE;
   ec->changes.shape_input = EINA_TRUE;

   if (!_e_comp_wl_client_new_helper(ec)) return;
   ec->wl_comp_data->first_damage = ((ec->internal) || (ec->override));

   eina_hash_add(clients_win_hash, &win, ec);
   e_hints_client_list_set();

   ec->wl_comp_data->first_draw_tmr = 
     ecore_timer_add(e_comp_config_get()->first_draw_delay, 
                     _e_comp_wl_cb_first_draw, ec);
}

static void 
_e_comp_wl_cb_hook_client_eval_fetch(void *data EINA_UNUSED, E_Client *ec)
{
   E_COMP_WL_PIXMAP_CHECK;

   /* DBG("COMP_WL HOOK CLIENT EVAL FETCH !!"); */

   if ((ec->changes.prop) || (ec->netwm.fetch.state))
     {
        e_hints_window_state_get(ec);
        ec->netwm.fetch.state = EINA_FALSE;
     }

   if ((ec->changes.prop) || (ec->e.fetch.state))
     {
        e_hints_window_e_state_get(ec);
        ec->e.fetch.state = EINA_FALSE;
     }

   if ((ec->changes.prop) || (ec->netwm.fetch.type))
     {
        e_hints_window_type_get(ec);
        if (((!ec->lock_border) || (!ec->border.name)) && 
            (ec->wl_comp_data->reparented))
          {
             ec->border.changed = EINA_TRUE;
             EC_CHANGED(ec);
          }

        if ((ec->netwm.type == E_WINDOW_TYPE_DOCK) || (ec->tooltip))
          {
             if (!ec->netwm.state.skip_pager)
               {
                  ec->netwm.state.skip_pager = EINA_TRUE;
                  ec->netwm.update.state = EINA_TRUE;
               }
             if (!ec->netwm.state.skip_taskbar)
               {
                  ec->netwm.state.skip_taskbar = EINA_TRUE;
                  ec->netwm.update.state = EINA_TRUE;
               }
          }
        else if (ec->netwm.type == E_WINDOW_TYPE_DESKTOP)
          {
             ec->focus_policy_override = E_FOCUS_CLICK;
             if (!ec->netwm.state.skip_pager)
               {
                  ec->netwm.state.skip_pager = EINA_TRUE;
                  ec->netwm.update.state = EINA_TRUE;
               }
             if (!ec->netwm.state.skip_taskbar)
               {
                  ec->netwm.state.skip_taskbar = EINA_TRUE;
                  ec->netwm.update.state = EINA_TRUE;
               }
             if (!e_client_util_ignored_get(ec))
               ec->border.changed = ec->borderless = EINA_TRUE;
          }

        if (ec->tooltip)
          {
             ec->icccm.accepts_focus = EINA_FALSE;
             eina_stringshare_replace(&ec->bordername, "borderless");
          }
        else if (ec->internal)
          {
             /* TODO: transient set */
          }

        E_Event_Client_Property *ev = E_NEW(E_Event_Client_Property, 1);
        ev->ec = ec;
        e_object_ref(E_OBJECT(ec));
        ev->property = E_CLIENT_PROPERTY_NETWM_STATE;
        ecore_event_add(E_EVENT_CLIENT_PROPERTY, ev, _e_comp_wl_client_event_free, NULL);

        ec->netwm.fetch.type = EINA_FALSE;
     }

   /* TODO: vkbd, etc */

   /* FIXME: Update ->changes.shape code for recent switch to eina_tiler */
   /* if (ec->changes.shape) */
   /*   { */
   /*      Eina_Rectangle *shape = NULL; */
   /*      Eina_Bool pshaped = EINA_FALSE; */

   /*      shape = eina_rectangle_new((ec->wl_comp_data->shape)->x,  */
   /*                                 (ec->wl_comp_data->shape)->y,  */
   /*                                 (ec->wl_comp_data->shape)->w,  */
   /*                                 (ec->wl_comp_data->shape)->h); */

   /*      pshaped = ec->shaped; */
   /*      ec->changes.shape = EINA_FALSE; */

   /*      if (eina_rectangle_is_empty(shape)) */
   /*        { */
   /*           if ((ec->shaped) && (ec->wl_comp_data->reparented) &&  */
   /*               (!ec->bordername)) */
   /*             { */
   /*                ec->border.changed = EINA_TRUE; */
   /*                EC_CHANGED(ec); */
   /*             } */

   /*           ec->shaped = EINA_FALSE; */
   /*        } */
   /*      else */
   /*        { */
   /*           int cw = 0, ch = 0; */

   /*           if (ec->border_size) */
   /*             { */
   /*                shape->x += ec->border_size; */
   /*                shape->y += ec->border_size; */
   /*                shape->w -= ec->border_size; */
   /*                shape->h -= ec->border_size; */
   /*             } */

   /*           e_pixmap_size_get(ec->pixmap, &cw, &ch); */
   /*           if ((cw != ec->client.w) || (ch != ec->client.h)) */
   /*             { */
   /*                ec->changes.shape = EINA_TRUE; */
   /*                EC_CHANGED(ec); */
   /*             } */

   /*           if ((shape->x == 0) && (shape->y == 0) &&  */
   /*               (shape->w == cw) && (shape->h == ch)) */
   /*             { */
   /*                if (ec->shaped) */
   /*                  { */
   /*                     ec->shaped = EINA_FALSE; */
   /*                     if ((ec->wl_comp_data->reparented) && (!ec->bordername)) */
   /*                       { */
   /*                          ec->border.changed = EINA_TRUE; */
   /*                          EC_CHANGED(ec); */
   /*                       } */
   /*                  } */
   /*             } */
   /*           else */
   /*             { */
   /*                if (ec->wl_comp_data->reparented) */
   /*                  { */
   /*                     EINA_RECTANGLE_SET(ec->wl_comp_data->shape,  */
   /*                                        shape->x, shape->y,  */
   /*                                        shape->w, shape->h); */

   /*                     if ((!ec->shaped) && (!ec->bordername)) */
   /*                       { */
   /*                          ec->border.changed = EINA_TRUE; */
   /*                          EC_CHANGED(ec); */
   /*                       } */
   /*                  } */
   /*                else */
   /*                  { */
   /*                     if (_e_comp_wl_client_shape_check(ec)) */
   /*                       e_comp_object_damage(ec->frame, 0, 0, ec->w, ec->h); */
   /*                  } */
   /*                ec->shaped = EINA_TRUE; */
   /*                ec->changes.shape_input = EINA_FALSE; */
   /*                ec->shape_input_rects_num = 0; */
   /*             } */

   /*           if (ec->shape_changed) */
   /*             e_comp_object_frame_theme_set(ec->frame,  */
   /*                                           E_COMP_OBJECT_FRAME_RESHADOW); */
   /*        } */

        /* if (ec->shaped != pshaped) */
        /*   { */
        /*      _e_comp_wl_client_shape_check(ec); */
        /*   } */

        /* ec->need_shape_merge = EINA_TRUE; */

        /* eina_rectangle_free(shape); */
     /* } */

   if ((ec->changes.prop) || (ec->netwm.update.state))
     {
        e_hints_window_state_set(ec);
        if (((!ec->lock_border) || (!ec->border.name)) && 
            (!(((ec->maximized & E_MAXIMIZE_TYPE) == E_MAXIMIZE_FULLSCREEN))) && 
               (ec->wl_comp_data->reparented))
          {
             ec->border.changed = EINA_TRUE;
             EC_CHANGED(ec);
          }

        if (ec->parent)
          {
             if (ec->netwm.state.modal)
               {
                  ec->parent->modal = ec;
                  if (ec->parent->focused)
                    evas_object_focus_set(ec->frame, EINA_TRUE);
               }
          }
        else if (ec->leader)
          {
             if (ec->netwm.state.modal)
               {
                  ec->leader->modal = ec;
                  if (ec->leader->focused)
                    evas_object_focus_set(ec->frame, EINA_TRUE);
                  else
                    {
                       Eina_List *l;
                       E_Client *child;

                       EINA_LIST_FOREACH(ec->leader->group, l, child)
                         {
                            if ((child != ec) && (child->focused))
                              evas_object_focus_set(ec->frame, EINA_TRUE);
                         }
                    }
               }
          }
        ec->netwm.update.state = EINA_FALSE;
     }

   ec->changes.prop = EINA_FALSE;
   if (!ec->wl_comp_data->reparented) ec->changes.border = EINA_FALSE;
   if (ec->changes.icon)
     {
        if (ec->wl_comp_data->reparented) return;
        ec->wl_comp_data->change_icon = EINA_TRUE;
        ec->changes.icon = EINA_FALSE;
     }
}

static void 
_e_comp_wl_cb_hook_client_pre_frame(void *data EINA_UNUSED, E_Client *ec)
{
   uint64_t parent;
   
   E_COMP_WL_PIXMAP_CHECK;

   if (!ec->wl_comp_data->need_reparent) return;

   WRN("Client Needs New Parent in Pre Frame");

   parent = e_client_util_pwin_get(ec);

   ec->border_size = 0;
   e_pixmap_parent_window_set(ec->pixmap, parent);
   ec->border.changed = EINA_TRUE;

   /* if (ec->shaped_input) */
   /*   { */
   /*      DBG("\tClient Shaped Input"); */
   /*   } */

   ec->changes.shape = EINA_TRUE;
   ec->changes.shape_input = EINA_TRUE;

   EC_CHANGED(ec);

   if (ec->visible)
     {
        /* FIXME: Other window types */
        if ((ec->wl_comp_data->set_win_type) && (ec->internal_ecore_evas))
          {
             E_Win *ewin;

             if ((ewin = ecore_evas_data_get(ec->internal_ecore_evas, "E_Win")))
               {
                  Ecore_Wl_Window *wwin;

                  wwin = ecore_evas_wayland_window_get(ec->internal_ecore_evas);
                  ecore_wl_window_type_set(wwin, ECORE_WL_WINDOW_TYPE_TOPLEVEL);
               }

             ec->wl_comp_data->set_win_type = EINA_FALSE;
          }
     }

   /* TODO */
   /* focus_setup */
   e_bindings_mouse_grab(E_BINDING_CONTEXT_WINDOW, parent);
   e_bindings_wheel_grab(E_BINDING_CONTEXT_WINDOW, parent);

   _e_comp_wl_client_evas_init(ec);

   if ((ec->netwm.ping) && (!ec->ping_poller)) 
     e_client_ping(ec);

   if (ec->visible) evas_object_show(ec->frame);

   ec->wl_comp_data->need_reparent = EINA_FALSE;
   ec->redirected = EINA_TRUE;

   if (ec->wl_comp_data->change_icon)
     {
        ec->changes.icon = EINA_TRUE;
        EC_CHANGED(ec);
     }

   ec->wl_comp_data->change_icon = EINA_FALSE;
   ec->wl_comp_data->reparented = EINA_TRUE;

   /* _e_comp_wl_evas_cb_comp_hidden(ec, NULL, NULL); */
}

static void 
_e_comp_wl_cb_hook_client_post_new(void *data EINA_UNUSED, E_Client *ec)
{
   E_COMP_WL_PIXMAP_CHECK;

   if (ec->need_shape_merge) ec->need_shape_merge = EINA_FALSE;

   if (ec->need_shape_export) 
     {
        _e_comp_wl_client_shape_check(ec);
        ec->need_shape_export = EINA_FALSE;
     }
}

static void 
_e_comp_wl_cb_hook_client_eval_end(void *data EINA_UNUSED, E_Client *ec)
{
   E_COMP_WL_PIXMAP_CHECK;

   if ((ec->comp->wl_comp_data->restack) && (!ec->comp->new_clients))
     {
        e_hints_client_stacking_set();
        ec->comp->wl_comp_data->restack = EINA_FALSE;
     }
}

static void 
_e_comp_wl_cb_hook_client_focus_set(void *data EINA_UNUSED, E_Client *ec)
{
   if ((!ec) || (!ec->wl_comp_data)) return;

   E_COMP_WL_PIXMAP_CHECK;

   /* FIXME: We cannot use e_grabinput_focus calls here */

   if (ec->wl_comp_data->shell.activate)
     {
        if (ec->wl_comp_data->shell.surface)
          ec->wl_comp_data->shell.activate(ec->wl_comp_data->shell.surface);
     }
}

static void 
_e_comp_wl_cb_hook_client_focus_unset(void *data EINA_UNUSED, E_Client *ec)
{
   if ((!ec) || (!ec->wl_comp_data)) return;
   E_COMP_WL_PIXMAP_CHECK;

   if (ec->wl_comp_data->shell.deactivate)
     {
        if (ec->wl_comp_data->shell.surface)
          ec->wl_comp_data->shell.deactivate(ec->wl_comp_data->shell.surface);
     }
}

EAPI Eina_Bool 
e_comp_wl_init(void)
{
   /* set gl available */
   if (ecore_evas_engine_type_supported_get(ECORE_EVAS_ENGINE_WAYLAND_EGL))
     e_comp_gl_set(EINA_TRUE);

   /* try to create a wayland compositor */
   if (!_e_comp_wl_compositor_create())
     {
        e_error_message_show(_("Enlightenment cannot create a Wayland Compositor!\n"));
        return EINA_FALSE;
     }

   /* set ecore_wayland in server mode
    * NB: this is done before init so that ecore_wayland does not stall while 
    * waiting for compositor to be created */
   /* ecore_wl_server_mode_set(EINA_TRUE); */

   /* try to initialize ecore_wayland */
   if (!ecore_wl_init(NULL))
     {
        e_error_message_show(_("Enlightenment cannot initialize Ecore_Wayland!\n"));
        return EINA_FALSE;
     }

   /* create hash to store client windows */
   clients_win_hash = eina_hash_int64_new(NULL);

   /* setup event handlers for e events */
   E_LIST_HANDLER_APPEND(handlers, E_EVENT_COMP_OBJECT_ADD, 
                         _e_comp_wl_cb_comp_object_add, NULL);
   /* E_LIST_HANDLER_APPEND(handlers, E_EVENT_CLIENT_ZONE_SET,  */
   /*                       _e_comp_wl_cb_client_zone_set, NULL); */
   E_LIST_HANDLER_APPEND(handlers, E_EVENT_CLIENT_PROPERTY, 
                         _e_comp_wl_cb_client_prop, NULL);

   /* setup client hooks */
   /* e_client_hook_add(E_CLIENT_HOOK_DESK_SET, _cb, NULL); */
   /* e_client_hook_add(E_CLIENT_HOOK_RESIZE_BEGIN, _cb, NULL); */
   /* e_client_hook_add(E_CLIENT_HOOK_RESIZE_END, _cb, NULL); */
   /* e_client_hook_add(E_CLIENT_HOOK_MOVE_BEGIN,  */
   /*                   _e_comp_wl_cb_hook_client_move_begin, NULL); */
   e_client_hook_add(E_CLIENT_HOOK_MOVE_END, 
                     _e_comp_wl_cb_hook_client_move_end, NULL);
   e_client_hook_add(E_CLIENT_HOOK_DEL, 
                     _e_comp_wl_cb_hook_client_del, NULL);
   e_client_hook_add(E_CLIENT_HOOK_NEW_CLIENT, 
                     _e_comp_wl_cb_hook_client_new, NULL);
   e_client_hook_add(E_CLIENT_HOOK_EVAL_FETCH, 
                     _e_comp_wl_cb_hook_client_eval_fetch, NULL);
   e_client_hook_add(E_CLIENT_HOOK_EVAL_PRE_FRAME_ASSIGN, 
                     _e_comp_wl_cb_hook_client_pre_frame, NULL);
   e_client_hook_add(E_CLIENT_HOOK_EVAL_POST_NEW_CLIENT, 
                     _e_comp_wl_cb_hook_client_post_new, NULL);
   e_client_hook_add(E_CLIENT_HOOK_EVAL_END, 
                     _e_comp_wl_cb_hook_client_eval_end, NULL);
   /* e_client_hook_add(E_CLIENT_HOOK_UNREDIRECT, _cb, NULL); */
   /* e_client_hook_add(E_CLIENT_HOOK_REDIRECT, _cb, NULL); */
   e_client_hook_add(E_CLIENT_HOOK_FOCUS_SET, 
                     _e_comp_wl_cb_hook_client_focus_set, NULL);
   e_client_hook_add(E_CLIENT_HOOK_FOCUS_UNSET, 
                     _e_comp_wl_cb_hook_client_focus_unset, NULL);

   /* TODO: e_desklock_hooks ?? */

   return EINA_TRUE;
}

EINTERN void 
e_comp_wl_shutdown(void)
{
   /* delete event handlers */
   E_FREE_LIST(handlers, ecore_event_handler_del);

   /* delete client window hash */
   E_FREE_FUNC(clients_win_hash, eina_hash_free);

   /* shutdown ecore_wayland */
   ecore_wl_shutdown();
}

EINTERN struct wl_resource *
e_comp_wl_surface_create(struct wl_client *client, int version, uint32_t id)
{
   struct wl_resource *ret = NULL;

   if ((ret = wl_resource_create(client, &wl_surface_interface, version, id)))
     wl_resource_set_implementation(ret, &_e_comp_wl_surface_interface, NULL, 
                                    e_comp_wl_surface_destroy);

   return ret;
}

EINTERN void 
e_comp_wl_surface_destroy(struct wl_resource *resource)
{
   E_Pixmap *cp;
   E_Client *ec;

   if (!(cp = wl_resource_get_user_data(resource))) return;

   /* try to find the E client for this surface */
   if (!(ec = e_pixmap_client_get(cp)))
     ec = e_pixmap_find_client(E_PIXMAP_TYPE_WL, e_pixmap_window_get(cp));

   if (!ec)
     {
        e_pixmap_free(cp);
        return;
     }

   e_object_del(E_OBJECT(ec));
}
