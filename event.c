/*
 * event.c - event handlers
 *
 * Copyright © 2007-2008 Julien Danjou <julien@danjou.info>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_event.h>

#include "awesome.h"
#include "event.h"
#include "objects/tag.h"
#include "xwindow.h"
#include "ewmh.h"
#include "objects/client.h"
#include "keyresolv.h"
#include "keygrabber.h"
#include "mousegrabber.h"
#include "luaa.h"
#include "systray.h"
#include "spawn.h"
#include "common/atoms.h"
#include "common/xutil.h"

#define DO_EVENT_HOOK_CALLBACK(type, xcbtype, xcbeventprefix, arraytype, match) \
    static void \
    event_##xcbtype##_callback(xcb_##xcbtype##_press_event_t *ev, \
                               arraytype *arr, \
                               int nargs, \
                               void *data) \
    { \
        int item_matching = 0; \
        foreach(item, *arr) \
            if(match(ev, *item, data)) \
            { \
                luaA_object_push(globalconf.L, *item); \
                item_matching++; \
            } \
        for(; item_matching > 0; item_matching--) \
        { \
            switch(ev->response_type) \
            { \
              case xcbeventprefix##_PRESS: \
                for(int i = 0; i < nargs; i++) \
                    lua_pushvalue(globalconf.L, - nargs - item_matching); \
                luaA_object_emit_signal(globalconf.L, - nargs - 1, "press", nargs); \
                break; \
              case xcbeventprefix##_RELEASE: \
                for(int i = 0; i < nargs; i++) \
                    lua_pushvalue(globalconf.L, - nargs - item_matching); \
                luaA_object_emit_signal(globalconf.L, - nargs - 1, "release", nargs); \
                break; \
            } \
            lua_pop(globalconf.L, 1); \
        } \
        lua_pop(globalconf.L, nargs); \
    }

static bool
event_key_match(xcb_key_press_event_t *ev, keyb_t *k, void *data)
{
    assert(data);
    xcb_keysym_t keysym = *(xcb_keysym_t *) data;
    return (((k->keycode && ev->detail == k->keycode)
             || (k->keysym && keysym == k->keysym))
            && (k->modifiers == XCB_BUTTON_MASK_ANY || k->modifiers == ev->state));
}

static bool
event_button_match(xcb_button_press_event_t *ev, button_t *b, void *data)
{
    return ((!b->button || ev->detail == b->button)
            && (b->modifiers == XCB_BUTTON_MASK_ANY || b->modifiers == ev->state));
}

DO_EVENT_HOOK_CALLBACK(button_t, button, XCB_BUTTON, button_array_t, event_button_match)
DO_EVENT_HOOK_CALLBACK(keyb_t, key, XCB_KEY, key_array_t, event_key_match)

static window_t *
window_getbywin(xcb_window_t window)
{
    if(_G_root->window == window)
        return _G_root;
    return (window_t *) ewindow_getbywin(window);
}

/** Handle an event with mouse grabber if needed
 * \param x The x coordinate.
 * \param y The y coordinate.
 * \param mask The mask buttons.
 * \return True if the event was handled.
 */
static bool
event_handle_mousegrabber(int x, int y, uint16_t mask)
{
    if(_G_mousegrabber)
    {
        luaA_object_push(globalconf.L, _G_mousegrabber);
        mousegrabber_handleevent(globalconf.L, x, y, mask);
        if(lua_pcall(globalconf.L, 1, 1, 0))
        {
            warn("error running function: %s", lua_tostring(globalconf.L, -1));
            luaA_mousegrabber_stop(globalconf.L);
        }
        else if(!lua_isboolean(globalconf.L, -1) || !lua_toboolean(globalconf.L, -1))
            luaA_mousegrabber_stop(globalconf.L);
        lua_pop(globalconf.L, 1);  /* pop returned value */
        return true;
    }
    return false;
}

/** The button press event handler.
 * \param data The type of mouse event.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_button(void *data, xcb_connection_t *connection, xcb_button_press_event_t *ev)
{
    if(event_handle_mousegrabber(ev->root_x, ev->root_y, 1 << (ev->detail - 1 + 8)))
        return 0;

    window_t *window = window_getbywin(ev->event);

    if(window)
    {
        luaA_object_push(globalconf.L, window);
        event_button_callback(ev, &window->buttons, 1, NULL);
    }

    return 0;
}

static void
event_handle_configurerequest_configure_window(xcb_configure_request_event_t *ev)
{
    uint16_t config_win_mask = 0;
    uint32_t config_win_vals[7];
    unsigned short i = 0;

    if(ev->value_mask & XCB_CONFIG_WINDOW_X)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_X;
        config_win_vals[i++] = ev->x;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_Y)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_Y;
        config_win_vals[i++] = ev->y;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_WIDTH;
        config_win_vals[i++] = ev->width;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_HEIGHT;
        config_win_vals[i++] = ev->height;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
        config_win_vals[i++] = ev->border_width;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_SIBLING)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_SIBLING;
        config_win_vals[i++] = ev->sibling;
    }
    if(ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
    {
        config_win_mask |= XCB_CONFIG_WINDOW_STACK_MODE;
        config_win_vals[i++] = ev->stack_mode;
    }

    xcb_configure_window(_G_connection, ev->window, config_win_mask, config_win_vals);
}

/** The configure event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_configurerequest(void *data __attribute__ ((unused)),
                              xcb_connection_t *connection, xcb_configure_request_event_t *ev)
{
    client_t *c;

    if((c = client_getbywin(ev->window)))
    {
        area_t geometry = c->geometry;

        if(ev->value_mask & XCB_CONFIG_WINDOW_X)
            geometry.x = ev->x;
        if(ev->value_mask & XCB_CONFIG_WINDOW_Y)
            geometry.y = ev->y;
        if(ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            geometry.width = ev->width;
        if(ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            geometry.height = ev->height;

        /* Push client */
        luaA_object_push(globalconf.L, c);

        if(ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
            ewindow_set_border_width(globalconf.L, -1, ev->border_width);

        if(!window_set_geometry(globalconf.L, -1, geometry))
            xwindow_configure(c->window, geometry, c->border_width);

        /* Remove client */
        lua_pop(globalconf.L, 1);
    }
    else
        event_handle_configurerequest_configure_window(ev);

    return 0;
}

/** The configure notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_configurenotify(void *data __attribute__ ((unused)),
                             xcb_connection_t *connection, xcb_configure_notify_event_t *ev)
{
    int screen_nbr;
    const xcb_screen_t *screen;

    for(screen_nbr = 0; screen_nbr < xcb_setup_roots_length(xcb_get_setup (connection)); screen_nbr++)
        if((screen = xutil_screen_get(connection, screen_nbr)) != NULL
           && ev->window == screen->root
           && (ev->width != screen->width_in_pixels
               || ev->height != screen->height_in_pixels))
            /* it's not that we panic, but restart */
            awesome_restart();

    return 0;
}

/** The destroy notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_destroynotify(void *data __attribute__ ((unused)),
                           xcb_connection_t *connection __attribute__ ((unused)),
                           xcb_destroy_notify_event_t *ev)
{
    client_t *c;

    if((c = client_getbywin(ev->window)))
        client_unmanage(c);
    else
        foreach(em, _G_embedded)
            if(em->window == ev->window)
            {
                xembed_window_array_remove(&_G_embedded, em);
                break;
            }

    return 0;
}

/** The motion notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_motionnotify(void *data __attribute__ ((unused)),
                          xcb_connection_t *connection,
                          xcb_motion_notify_event_t *ev)
{
    event_handle_mousegrabber(ev->root_x, ev->root_y, ev->state);
    return 0;
}

/** The leave notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_leavenotify(void *data __attribute__ ((unused)),
                         xcb_connection_t *connection,
                         xcb_leave_notify_event_t *ev)
{
    if(ev->mode != XCB_NOTIFY_MODE_NORMAL)
        return 0;

    window_t *window = window_getbywin(ev->event);

    if(window)
    {
        luaA_object_push(globalconf.L, window);
        luaA_object_emit_signal(globalconf.L, -1, "mouse::leave", 0);
        lua_pop(globalconf.L, 1);
    }

    return 0;
}

/** The enter notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_enternotify(void *data __attribute__ ((unused)),
                         xcb_connection_t *connection,
                         xcb_enter_notify_event_t *ev)
{
    if(ev->mode != XCB_NOTIFY_MODE_NORMAL)
        return 0;

    window_t *window = window_getbywin(ev->event);

    if(window)
    {
        luaA_object_push(globalconf.L, window);
        luaA_object_emit_signal(globalconf.L, -1, "mouse::enter", 0);
        lua_pop(globalconf.L, 1);
    }

    return 0;
}

/** The focus in event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_focusin(void *data __attribute__ ((unused)),
                     xcb_connection_t *connection,
                     xcb_focus_in_event_t *ev)
{
    /* Events that we are interested in: */
    switch(ev->detail)
    {
        /* These are events that jump between root windows.
         */
        case XCB_NOTIFY_DETAIL_ANCESTOR:
        case XCB_NOTIFY_DETAIL_INFERIOR:

        /* These are events that jump between clients.
         * Virtual events ensure we always get an event on our top-level window.
         */
        case XCB_NOTIFY_DETAIL_NONLINEAR_VIRTUAL:
        case XCB_NOTIFY_DETAIL_NONLINEAR:
          {
            window_t *window = window_getbywin(ev->event);

            if(window)
                window_focus_update(window);
          }
        /* all other events are ignored */
        default:
            break;
    }
    return 0;
}

/** The focus out event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_focusout(void *data __attribute__ ((unused)),
                      xcb_connection_t *connection,
                      xcb_focus_in_event_t *ev)
{
    /* Events that we are interested in: */
    switch(ev->detail)
    {
        /* These are events that jump between root windows.
         */
        case XCB_NOTIFY_DETAIL_ANCESTOR:
        case XCB_NOTIFY_DETAIL_INFERIOR:

        /* These are events that jump between clients.
         * Virtual events ensure we always get an event on our top-level window.
         */
        case XCB_NOTIFY_DETAIL_NONLINEAR_VIRTUAL:
        case XCB_NOTIFY_DETAIL_NONLINEAR:
          {
            window_t *window = window_getbywin(ev->event);

            if(window)
                window_unfocus_update((window_t *) window);
          }
        /* all other events are ignored */
        default:
            break;
    }
    return 0;
}

/** The expose event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_expose(void *data __attribute__ ((unused)),
                    xcb_connection_t *connection __attribute__ ((unused)),
                    xcb_expose_event_t *ev)
{
    wibox_t *wibox;

    /* If the wibox got need_update set, skip this because it will be repainted
     * soon anyway. Without this we could be painting garbage to the screen!
     */
    if((wibox = wibox_getbywin(ev->window)) && !wibox->need_update)
        wibox_refresh_pixmap_partial(wibox,
                                     ev->x, ev->y,
                                     ev->width, ev->height);

    return 0;
}

/** The key press event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_key(void *data __attribute__ ((unused)),
                 xcb_connection_t *connection __attribute__ ((unused)),
                 xcb_key_press_event_t *ev)
{
    if(_G_keygrabber)
    {
        luaA_object_push(globalconf.L, _G_keygrabber);
        if(keygrabber_handlekpress(globalconf.L, ev))
        {
            if(lua_pcall(globalconf.L, 3, 1, 0))
            {
                warn("error running function: %s", lua_tostring(globalconf.L, -1));
                luaA_keygrabber_stop(globalconf.L);
            }
            else if(!lua_isboolean(globalconf.L, -1) || !lua_toboolean(globalconf.L, -1))
                luaA_keygrabber_stop(globalconf.L);
        }
        lua_pop(globalconf.L, 1);  /* pop returned value or function if not called */
    }
    else
    {
        /* get keysym ignoring all modifiers */
        xcb_keysym_t keysym = keyresolv_get_keysym(ev->detail, 0);
        window_t *window = window_getbywin(ev->event);
        if(window)
        {
            luaA_object_push(globalconf.L, window);
            event_key_callback(ev, &window->keys, 1, &keysym);
        }
    }

    return 0;
}

/** The map request event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_maprequest(void *data __attribute__ ((unused)),
                        xcb_connection_t *connection, xcb_map_request_event_t *ev)
{
    int ret = 0;
    client_t *c;
    xcb_get_window_attributes_cookie_t wa_c;
    xcb_get_window_attributes_reply_t *wa_r;
    xcb_get_geometry_cookie_t geom_c;
    xcb_get_geometry_reply_t *geom_r;

    wa_c = xcb_get_window_attributes_unchecked(connection, ev->window);

    if(!(wa_r = xcb_get_window_attributes_reply(connection, wa_c, NULL)))
        return -1;

    if(wa_r->override_redirect)
        goto bailout;

    if(xembed_getbywin(&_G_embedded, ev->window))
    {
        xcb_map_window(connection, ev->window);
        xembed_window_activate(connection, ev->window);
        goto bailout;
    }

    if((c = client_getbywin(ev->window)))
    {
        /* Check that it may be visible, but not asked to be hidden */
        if(ewindow_isvisible((ewindow_t *) c))
        {
            luaA_object_push(globalconf.L, c);
            ewindow_set_minimized(globalconf.L, -1, false);
            /* it will be raised, so just update ourself */
            stack_window_raise(globalconf.L, -1);
            lua_pop(globalconf.L, 1);
        }
    }
    else
    {
        geom_c = xcb_get_geometry_unchecked(connection, ev->window);

        if(!(geom_r = xcb_get_geometry_reply(connection, geom_c, NULL)))
        {
            ret = -1;
            goto bailout;
        }

        client_manage(ev->window, geom_r, false);

        p_delete(&geom_r);
    }

bailout:
    p_delete(&wa_r);
    return ret;
}

/** The unmap notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_unmapnotify(void *data __attribute__ ((unused)),
                         xcb_connection_t *connection, xcb_unmap_notify_event_t *ev)
{
    client_t *c;

    if((c = client_getbywin(ev->window)))
    {
        if(ev->event == _G_root->window
           && XCB_EVENT_SENT(ev)
           && xwindow_get_state_reply(xwindow_get_state_unchecked(c->window)) == XCB_WM_STATE_NORMAL)
            client_unmanage(c);
    }
    else
        foreach(em, _G_embedded)
            if(em->window == ev->window)
            {
                xembed_window_array_remove(&_G_embedded, em);
                break;
            }

    return 0;
}

/** The randr screen change notify event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_randr_screen_change_notify(void *data __attribute__ ((unused)),
                                        xcb_connection_t *connection __attribute__ ((unused)),
                                        xcb_randr_screen_change_notify_event_t *ev)
{
    /* Code  of  XRRUpdateConfiguration Xlib  function  ported to  XCB
     * (only the code relevant  to RRScreenChangeNotify) as the latter
     * doesn't provide this kind of function */
    if(ev->rotation & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270))
        xcb_randr_set_screen_size(connection, ev->root, ev->height, ev->width,
                                  ev->mheight, ev->mwidth);
    else
        xcb_randr_set_screen_size(connection, ev->root, ev->width, ev->height,
                                  ev->mwidth, ev->mheight);

    /* XRRUpdateConfiguration also executes the following instruction
     * but it's not useful because SubpixelOrder is not used at all at
     * the moment
     *
     * XRenderSetSubpixelOrder(dpy, snum, scevent->subpixel_order);
     */

    awesome_restart();

    return 0;
}

/** The client message event handler.
 * \param data currently unused.
 * \param connection The connection to the X server.
 * \param ev The event.
 */
static int
event_handle_clientmessage(void *data __attribute__ ((unused)),
                           xcb_connection_t *connection,
                           xcb_client_message_event_t *ev)
{
    /* check for startup notification messages */
    if(sn_xcb_display_process_event(_G_sndisplay, (xcb_generic_event_t *) ev))
        return 0;

    if(ev->type == WM_CHANGE_STATE)
    {
        client_t *c;
        if((c = client_getbywin(ev->window))
           && ev->format == 32
           && ev->data.data32[0] == XCB_WM_STATE_ICONIC)
        {
            luaA_object_push(globalconf.L, c);
            ewindow_set_minimized(globalconf.L, -1, true);
            lua_pop(globalconf.L, 1);
        }
    }
    else if(ev->type == _XEMBED)
        return xembed_process_client_message(ev);
    else if(ev->type == _NET_SYSTEM_TRAY_OPCODE)
        return systray_process_client_message(ev);
    return ewmh_process_client_message(ev);
}

/** The keymap change notify event handler.
 * \param data Unused data.
 * \param connection The connection to the X server.
 * \param ev The event.
 * \return Status code, 0 if everything's fine.
 */
static int
event_handle_mappingnotify(void *data,
                           xcb_connection_t *connection,
                           xcb_mapping_notify_event_t *ev)
{
    if(ev->request == XCB_MAPPING_MODIFIER
       || ev->request == XCB_MAPPING_KEYBOARD)
    {
        xcb_get_modifier_mapping_cookie_t xmapping_cookie =
            xcb_get_modifier_mapping_unchecked(_G_connection);

        /* Free and then allocate the key symbols */
        xcb_key_symbols_free(globalconf.keysyms);
        globalconf.keysyms = xcb_key_symbols_alloc(_G_connection);

        keyresolv_lock_mask_refresh(_G_connection, xmapping_cookie, globalconf.keysyms);

        /* regrab everything */
        xwindow_grabkeys(_G_root->window, &_G_root->keys);

        foreach(c, globalconf.clients)
            xwindow_grabkeys((*c)->window, &(*c)->keys);
    }

    return 0;
}

static int
event_handle_reparentnotify(void *data,
                           xcb_connection_t *connection,
                           xcb_reparent_notify_event_t *ev)
{
    client_t *c;

    if((c = client_getbywin(ev->window)))
        client_unmanage(c);

    return 0;
}

void a_xcb_set_event_handlers(void)
{
    const xcb_query_extension_reply_t *randr_query;

    xcb_event_set_button_press_handler(&_G_evenths, event_handle_button, NULL);
    xcb_event_set_button_release_handler(&_G_evenths, event_handle_button, NULL);
    xcb_event_set_configure_request_handler(&_G_evenths, event_handle_configurerequest, NULL);
    xcb_event_set_configure_notify_handler(&_G_evenths, event_handle_configurenotify, NULL);
    xcb_event_set_destroy_notify_handler(&_G_evenths, event_handle_destroynotify, NULL);
    xcb_event_set_enter_notify_handler(&_G_evenths, event_handle_enternotify, NULL);
    xcb_event_set_leave_notify_handler(&_G_evenths, event_handle_leavenotify, NULL);
    xcb_event_set_focus_in_handler(&_G_evenths, event_handle_focusin, NULL);
    xcb_event_set_focus_out_handler(&_G_evenths, event_handle_focusout, NULL);
    xcb_event_set_motion_notify_handler(&_G_evenths, event_handle_motionnotify, NULL);
    xcb_event_set_expose_handler(&_G_evenths, event_handle_expose, NULL);
    xcb_event_set_key_press_handler(&_G_evenths, event_handle_key, NULL);
    xcb_event_set_key_release_handler(&_G_evenths, event_handle_key, NULL);
    xcb_event_set_map_request_handler(&_G_evenths, event_handle_maprequest, NULL);
    xcb_event_set_unmap_notify_handler(&_G_evenths, event_handle_unmapnotify, NULL);
    xcb_event_set_client_message_handler(&_G_evenths, event_handle_clientmessage, NULL);
    xcb_event_set_mapping_notify_handler(&_G_evenths, event_handle_mappingnotify, NULL);
    xcb_event_set_reparent_notify_handler(&_G_evenths, event_handle_reparentnotify, NULL);

    /* check for randr extension */
    randr_query = xcb_get_extension_data(_G_connection, &xcb_randr_id);
    if(randr_query->present)
        xcb_event_set_handler(&_G_evenths,
                              randr_query->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY,
                              (xcb_generic_event_handler_t) event_handle_randr_screen_change_notify,
                              NULL);

}

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=80
