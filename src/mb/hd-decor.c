/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author: Thomas Thurman <thomas.thurman@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "hd-decor.h"
#include "hd-theme.h"
#include "hd-comp-mgr.h"
#include "hd-app.h"

#include "hd-title-bar.h"
#include "hd-render-manager.h"
#include "hd-clutter-cache.h"
#include "hd-transition.h"
#include "hd-gtk-style.h"

#include <matchbox/theme-engines/mb-wm-theme-png.h>
#include <matchbox/theme-engines/mb-wm-theme-xml.h>

#define HD_DECOR_TITLE_MARGIN 24


static void
hd_decor_remove_actors(HdDecor   *decor);

static void
hd_decor_class_init (MBWMObjectClass *klass)
{
  /* MBWMDecorClass *d_class = MB_WM_DECOR_CLASS (klass); */

#if MBWM_WANT_DEBUG
  klass->klass_name = "HdDecor";
#endif
}

static void
hd_decor_destroy (MBWMObject *obj)
{
  HdDecor *decor = HD_DECOR (obj);

  if (decor->progress_timeline)
    {
      clutter_timeline_stop(decor->progress_timeline);
      g_object_unref(decor->progress_timeline);
      decor->progress_timeline = 0;
    }
  /* we still want them inside the window we put them in */
  decor->progress_texture = 0;
  decor->title_bar_actor = 0;
  decor->title_actor = 0;
}

static int
hd_decor_init (MBWMObject *obj, va_list vap)
{
  HdDecor *d = HD_DECOR (obj);

  d->progress_timeline = 0;
  d->progress_texture = 0;
  d->title_bar_actor = 0;
  d->title_actor = 0;

  return 1;
}

HdDecor*
hd_decor_new (MBWindowManager      *wm,
              MBWMDecorType         type)
{
  MBWMObject *decor;

  decor = mb_wm_object_new (HD_TYPE_DECOR,
                            MBWMObjectPropWm,               wm,
                            MBWMObjectPropDecorType,        type,
                            NULL);

  return HD_DECOR(decor);
}

int
hd_decor_class_type ()
{
  static int type = 0;

  if (UNLIKELY(type == 0))
    {
      static MBWMObjectClassInfo info = {
	sizeof (HdDecorClass),
	sizeof (HdDecor),
	hd_decor_init,
	hd_decor_destroy,
	hd_decor_class_init
      };

      type = mb_wm_object_register_class (&info, MB_WM_TYPE_DECOR, 0);
    }

  return type;
}

static gboolean
hd_decor_window_check_prop (MBWindowManager *wm, Window w, HdAtoms atom)
{
  HdCompMgr *hmgr = HD_COMP_MGR (wm->comp_mgr);
  Atom progress_indicator =
      hd_comp_mgr_get_atom (hmgr, atom);
  Atom actual_type_return;
  int actual_format_return;
  unsigned long nitems_return;
  unsigned long bytes_after_return;
  unsigned char* prop_return = NULL;
  int result = 0;

  mb_wm_util_async_trap_x_errors(wm->xdpy);
  XGetWindowProperty (wm->xdpy, w,
                      progress_indicator,
                      0, G_MAXLONG,
                      False,
                      AnyPropertyType,
                      &actual_type_return,
                      &actual_format_return,
                      &nitems_return,
                      &bytes_after_return,
                      &prop_return);
  if (prop_return)
    {
      result = prop_return[0];
      XFree (prop_return);
    }
  mb_wm_util_async_untrap_x_errors();

  return result;
}

gboolean
hd_decor_window_is_waiting (MBWindowManager *wm, Window w)
{
  return hd_decor_window_check_prop(wm, w,
      HD_ATOM_HILDON_WM_WINDOW_PROGRESS_INDICATOR);
}

gboolean
hd_decor_window_has_menu_indicator (MBWindowManager *wm, Window w)
{
  return hd_decor_window_check_prop(wm, w,
      HD_ATOM_HILDON_WM_WINDOW_MENU_INDICATOR);
}

static
MBWindowManagerClient *hd_decor_get_client(HdDecor   *decor)
{
  if (!decor)
    return NULL;

  return MB_WM_DECOR(decor)->parent_client;
}

static ClutterActor *
hd_decor_get_actor(HdDecor   *decor)
{
  MBWindowManagerClient* client = hd_decor_get_client(decor);
  if (!client || !client->cm_client)
    return NULL;
  return mb_wm_comp_mgr_clutter_client_get_actor(
            MB_WM_COMP_MGR_CLUTTER_CLIENT(client->cm_client));
}

static void
hd_decor_remove_actors(HdDecor   *decor)
{
  ClutterActor      *actor = hd_decor_get_actor(decor);

  if (decor->progress_timeline)
    {
      clutter_timeline_stop(decor->progress_timeline);
      g_object_unref(decor->progress_timeline);
      decor->progress_timeline = 0;
    }
  if (decor->progress_texture)
    {
      clutter_actor_remove_child(actor, decor->progress_texture);
      decor->progress_texture = 0;
    }
  if (decor->title_bar_actor)
    {
      clutter_actor_remove_child(actor, decor->title_bar_actor);
      decor->title_bar_actor = 0;
    }
  if (decor->title_actor)
    {
      clutter_actor_remove_child(actor, decor->title_actor);
      decor->title_actor = 0;
    }
}

/* Fill the ClutterActor for the given decor with the correct
 * clutter actors to display the title */
static void
hd_decor_create_actors(HdDecor *decor)
{
  MBWMDecor         *mb_decor = MB_WM_DECOR (decor);
  ClutterActor      *actor = hd_decor_get_actor(decor);
  MBWindowManagerClient  *client = mb_decor->parent_client;
  MBWMClientType          c_type;
  MBWMXmlClient     *c;
  MBWMXmlDecor      *d;
  ClutterGeometry   /*geo, */area;
  gboolean          is_waiting = FALSE;

  if (!client)
    return;

  c_type = MB_WM_CLIENT_CLIENT_TYPE (client);

  if (!((c = mb_wm_xml_client_find_by_type
                      (client->wmref->theme->xml_clients, c_type)) &&
        (d = mb_wm_xml_decor_find_by_type (c->decors, mb_decor->type))))
    return;

  area.x = 0;
  area.y = 0;
  area.width = mb_decor->geom.width;
  area.height = mb_decor->geom.height;

  if (c->image_filename)
    {
      ClutterGeometry geo = {d->x, d->y, d->width, d->height};
      decor->title_bar_actor = hd_clutter_cache_get_sub_texture_for_area(
                                  c->image_filename, TRUE, &geo, &area);
    }
  else
    {
      decor->title_bar_actor = hd_clutter_cache_get_texture_for_area(
                                  HD_THEME_IMG_DIALOG_BAR, TRUE, &area);
    }
  /* If clients don't have a frame, the actor will be positioned according to
   * the normal window - so we need to correct for this. */
  if (client->xwin_frame)
    clutter_actor_set_position(decor->title_bar_actor,
            mb_decor->geom.x, mb_decor->geom.y);
  else
    clutter_actor_set_position(decor->title_bar_actor,
              mb_decor->geom.x+client->frame_geometry.x-client->window->geometry.x,
              mb_decor->geom.y+client->frame_geometry.y-client->window->geometry.y);

  clutter_actor_add_child(CLUTTER_ACTOR(actor), decor->title_bar_actor);

  /* add the title */
  if (d->show_title)
    {
      const char* title = mb_wm_client_get_name (client);
      if (title && strlen(title)) {
        ClutterText *bar_title;
        ClutterColor default_color = { 0xFF, 0xFF, 0xFF, 0xFF };
        char font_name[512];
        gfloat w = 0, h = 0;
        int screen_width_avail = hd_comp_mgr_get_current_screen_width ();
        if (is_waiting)
          screen_width_avail -= HD_THEME_IMG_PROGRESS_SIZE+
                                HD_TITLE_BAR_PROGRESS_MARGIN;

        hd_gtk_style_get_fg_color(HD_GTK_BUTTON_SINGLETON,
                                  GTK_STATE_NORMAL, &default_color);

        /* TODO: handle it so that _NET_WM_NAME has pure UTF-8 and no markup,
         * and _HILDON_WM_NAME has UTF-8 + Pango markup. If _HILDON_WM_NAME
         * is there, it is used, otherwise use the traditional properties. */
        bar_title = CLUTTER_TEXT(clutter_text_new());
        clutter_text_set_color(bar_title, &default_color);

        /* set Pango markup only if the string is XML fragment */
        if (client->window->name_has_markup)
          clutter_text_set_use_markup(bar_title, TRUE);

        decor->title_actor = CLUTTER_ACTOR(bar_title);
        clutter_actor_add_child(CLUTTER_ACTOR(actor), decor->title_actor);

        snprintf (font_name, sizeof (font_name), "%s %i%s",
                  d->font_family ? d->font_family : "Sans",
                  d->font_size ? d->font_size : 18,
                  d->font_units == MBWMXmlFontUnitsPoints ? "" : "px");
        clutter_text_set_font_name(bar_title, font_name);
        clutter_text_set_text(bar_title, title);

        clutter_actor_get_size(CLUTTER_ACTOR(bar_title), &w, &h);
        /* if it's too big, make sure we crop it */
        if (w > screen_width_avail)
          {
            clutter_text_set_ellipsize(bar_title, PANGO_ELLIPSIZE_NONE);
            clutter_actor_set_width(CLUTTER_ACTOR(bar_title),
                                    screen_width_avail);
            clutter_actor_set_clip(CLUTTER_ACTOR(bar_title),
                                   0, 0,
                                   screen_width_avail, h);
            w = screen_width_avail;
          }

        clutter_actor_set_position(CLUTTER_ACTOR(bar_title),
            (screen_width_avail - w) / 2,
            (mb_decor->geom.height - h) / 2);
      }
      /* Check whether we should be displaying a waiting animation. We
       * only want this is we have a title. */
      is_waiting = hd_decor_window_is_waiting(client->wmref,
                                              client->window->xwindow);
    }

  /* Add the progress indicator if required */
  if (is_waiting)
    {
      /* Get the actor we're going to rotate and put it on the right-hand
       * side of the window*/
      ClutterGeometry progress_geo =
        {0, 0, HD_THEME_IMG_PROGRESS_SIZE, HD_THEME_IMG_PROGRESS_SIZE};
      gint x = 0;
      decor->progress_texture = hd_clutter_cache_get_sub_texture(
                            HD_THEME_IMG_PROGRESS, TRUE, &progress_geo);
      if (decor->title_actor)
        {
          x = clutter_actor_get_x(CLUTTER_ACTOR(decor->title_actor)) +
              clutter_actor_get_width(CLUTTER_ACTOR(decor->title_actor)) +
              HD_TITLE_BAR_PROGRESS_MARGIN;
        }
      clutter_actor_add_child(CLUTTER_ACTOR(actor), decor->progress_texture);
      clutter_actor_set_position(decor->progress_texture,
          x,
          (mb_decor->geom.height - HD_THEME_IMG_PROGRESS_SIZE)/2);
      clutter_actor_set_size(decor->progress_texture,
          HD_THEME_IMG_PROGRESS_SIZE, HD_THEME_IMG_PROGRESS_SIZE);
      /* Get the timeline and set it running */
      decor->progress_timeline = g_object_ref(
          clutter_timeline_new(1000 * HD_THEME_IMG_PROGRESS_FRAMES /
                               HD_THEME_IMG_PROGRESS_FPS));
      clutter_timeline_set_repeat_count(decor->progress_timeline, -1);
      g_signal_connect (decor->progress_timeline, "new-frame",
                        G_CALLBACK (on_decor_progress_timeline_new_frame),
                        decor->progress_texture);
      clutter_timeline_start(decor->progress_timeline);
    }
}

void hd_decor_sync(HdDecor *decor)
{
  MBWMDecor *mbdecor = MB_WM_DECOR(decor);
  MBWindowManagerClient  *client = MB_WM_DECOR(decor)->parent_client;
  ClutterActor *actor;
  HdTitleBar *bar;

  if (!client || !client->wmref)
    return;

  bar = HD_TITLE_BAR(hd_render_manager_get_title_bar());
  if (bar && hd_title_bar_is_title_bar_decor(bar, mbdecor))
    hd_title_bar_update(bar);

  actor = hd_decor_get_actor(decor);
  if (!actor)
      return;

  /* TODO: We probably want to try and adjust the current actors
   * rather than removing them and recreating them. */
  hd_decor_remove_actors(decor);

  if (MB_WM_DECOR(decor)->geom.width > 0 &&
      MB_WM_DECOR(decor)->geom.height > 0 &&
      MB_WM_CLIENT_CLIENT_TYPE(client) != MBWMClientTypeApp)
    {
      /* For dialogs, etc. We need to fill our clutter group with
       * all the actors needed to draw it. */
      hd_decor_create_actors(decor);
    }
}

