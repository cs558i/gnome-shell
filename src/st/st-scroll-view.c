/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * st-scroll-view.h: Container with scroll-bars
 *
 * Copyright 2008 OpenedHand
 * Copyright 2009 Intel Corporation.
 * Copyright 2009, 2010 Red Hat, Inc.
 * Copyright 2010 Maxim Ermilov
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:st-scroll-view
 * @short_description: a container for scrollable children
 *
 * #StScrollView is a single child container for actors that implement
 * #StScrollable. It provides scrollbars around the edge of the child to
 * allow the user to move around the scrollable area.
 */

/* TODO: The code here currently only deals with height-for-width
 * allocation; width-for-height allocation would need a second set of
 * code paths through get_preferred_height()/get_preferred_width()/allocate()
 * that reverse the roles of the horizontal and vertical scrollbars.
 *
 * TODO: The multiple layout passes with and without scrollbars when
 * using the automatic policy causes considerable inefficiency because
 * it breaks request caching; we should saved the last size passed
 * into allocate() and if it's the same as previous size not repeat
 * the determination of scrollbar visibility. This requires overriding
 * queue_relayout() so we know when to discard the saved value.
 *
 * The size negotiation between the #StScrollView and the child is
 * described in the documentation for #StScrollable; the significant
 * part to note there is that reported minimum sizes for a scrolled
 * child are the minimum sizes when no scrollbar is needed. This allows
 * us to determine what scrollbars are visible without a need to look
 * inside the #StAdjustment.
 *
 * The second simplification that we make that allows us to implement
 * a straightforward height-for-width negotiation without multiple
 * allocate passes is that when the scrollbar policy is
 * AUTO, we always reserve space for the scrollbar in the
 * reported minimum and natural size.
 *
 * See https://bugzilla.gnome.org/show_bug.cgi?id=611740 for a more
 * detailed description of the considerations involved.
 */

#include "st-enum-types.h"
#include "st-private.h"
#include "st-scroll-view.h"
#include "st-scroll-bar.h"
#include "st-scrollable.h"
#include "st-scroll-view-fade.h"
#include <clutter/clutter.h>
#include <math.h>

static void clutter_container_iface_init (ClutterContainerIface *iface);

static ClutterContainerIface *st_scroll_view_parent_iface = NULL;

struct _StScrollViewPrivate
{
  /* a pointer to the child; this is actually stored
   * inside StBin:child, but we keep it to avoid
   * calling st_bin_get_child() every time we need it
   */
  ClutterActor *child;

  StAdjustment *hadjustment;
  ClutterActor *hscroll;
  StAdjustment *vadjustment;
  ClutterActor *vscroll;

  StPolicyType hscrollbar_policy;
  StPolicyType vscrollbar_policy;

  gfloat        row_size;
  gfloat        column_size;

  StScrollViewFade *fade_effect;

  guint         row_size_set : 1;
  guint         column_size_set : 1;
  guint         mouse_scroll : 1;
  guint         overlay_scrollbars : 1;
  guint         hscrollbar_visible : 1;
  guint         vscrollbar_visible : 1;
};

G_DEFINE_TYPE_WITH_CODE (StScrollView, st_scroll_view, ST_TYPE_BIN,
                         G_ADD_PRIVATE (StScrollView)
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_CONTAINER,
                                                clutter_container_iface_init))

enum {
  PROP_0,

  PROP_HSCROLL,
  PROP_VSCROLL,
  PROP_HSCROLLBAR_POLICY,
  PROP_VSCROLLBAR_POLICY,
  PROP_HSCROLLBAR_VISIBLE,
  PROP_VSCROLLBAR_VISIBLE,
  PROP_MOUSE_SCROLL,
  PROP_OVERLAY_SCROLLBARS,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

static void
st_scroll_view_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  StScrollViewPrivate *priv = ((StScrollView *) object)->priv;

  switch (property_id)
    {
    case PROP_HSCROLL:
      g_value_set_object (value, priv->hscroll);
      break;
    case PROP_VSCROLL:
      g_value_set_object (value, priv->vscroll);
      break;
    case PROP_HSCROLLBAR_POLICY:
      g_value_set_enum (value, priv->hscrollbar_policy);
      break;
    case PROP_VSCROLLBAR_POLICY:
      g_value_set_enum (value, priv->vscrollbar_policy);
      break;
    case PROP_HSCROLLBAR_VISIBLE:
      g_value_set_boolean (value, priv->hscrollbar_visible);
      break;
    case PROP_VSCROLLBAR_VISIBLE:
      g_value_set_boolean (value, priv->vscrollbar_visible);
      break;
    case PROP_MOUSE_SCROLL:
      g_value_set_boolean (value, priv->mouse_scroll);
      break;
    case PROP_OVERLAY_SCROLLBARS:
      g_value_set_boolean (value, priv->overlay_scrollbars);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

/**
 * st_scroll_view_update_fade_effect:
 * @scroll: a #StScrollView
 * @fade_margins: a #ClutterMargin defining the vertical fade effects, in pixels.
 *
 * Sets the fade effects in all four edges of the view. A value of 0
 * disables the effect.
 */
void
st_scroll_view_update_fade_effect (StScrollView  *scroll,
                                   ClutterMargin *fade_margins)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (scroll)->priv;

  /* A fade amount of other than 0 enables the effect. */
  if (fade_margins->left != 0. || fade_margins->right != 0. ||
      fade_margins->top != 0. || fade_margins->bottom != 0.)
    {
      if (priv->fade_effect == NULL)
        {
          priv->fade_effect = g_object_new (ST_TYPE_SCROLL_VIEW_FADE, NULL);

          clutter_actor_add_effect_with_name (CLUTTER_ACTOR (scroll), "fade",
                                              CLUTTER_EFFECT (priv->fade_effect));
        }

      g_object_set (priv->fade_effect,
                    "fade-margins", fade_margins,
                    NULL);
    }
   else
    {
      if (priv->fade_effect != NULL)
        {
          clutter_actor_remove_effect (CLUTTER_ACTOR (scroll),
                                       CLUTTER_EFFECT (priv->fade_effect));
          priv->fade_effect = NULL;
        }
    }
}

static void
st_scroll_view_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  StScrollView *self = ST_SCROLL_VIEW (object);
  StScrollViewPrivate *priv = self->priv;

  switch (property_id)
    {
    case PROP_MOUSE_SCROLL:
      st_scroll_view_set_mouse_scrolling (self,
                                          g_value_get_boolean (value));
      break;
    case PROP_OVERLAY_SCROLLBARS:
      st_scroll_view_set_overlay_scrollbars (self,
                                             g_value_get_boolean (value));
      break;
    case PROP_HSCROLLBAR_POLICY:
      st_scroll_view_set_policy (self,
                                 g_value_get_enum (value),
                                 priv->vscrollbar_policy);
      break;
    case PROP_VSCROLLBAR_POLICY:
      st_scroll_view_set_policy (self,
                                 priv->hscrollbar_policy,
                                 g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
st_scroll_view_dispose (GObject *object)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (object)->priv;

  if (priv->fade_effect)
    {
      clutter_actor_remove_effect (CLUTTER_ACTOR (object), CLUTTER_EFFECT (priv->fade_effect));
      priv->fade_effect = NULL;
    }

  g_clear_pointer (&priv->vscroll, clutter_actor_destroy);
  g_clear_pointer (&priv->hscroll, clutter_actor_destroy);

  /* For most reliable freeing of memory, an object with signals
   * like StAdjustment should be explicitly disposed. Since we own
   * the adjustments, we take care of that. This also disconnects
   * the signal handlers that we established on creation.
   */
  if (priv->hadjustment)
    {
      g_object_run_dispose (G_OBJECT (priv->hadjustment));
      g_object_unref (priv->hadjustment);
      priv->hadjustment = NULL;
    }

  if (priv->vadjustment)
    {
      g_object_run_dispose (G_OBJECT (priv->vadjustment));
      g_object_unref (priv->vadjustment);
      priv->vadjustment = NULL;
    }

  G_OBJECT_CLASS (st_scroll_view_parent_class)->dispose (object);
}

static void
st_scroll_view_paint (ClutterActor        *actor,
                      ClutterPaintContext *paint_context)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (actor)->priv;

  st_widget_paint_background (ST_WIDGET (actor), paint_context);

  if (priv->child)
    clutter_actor_paint (priv->child, paint_context);
  if (priv->hscrollbar_visible)
    clutter_actor_paint (priv->hscroll, paint_context);
  if (priv->vscrollbar_visible)
    clutter_actor_paint (priv->vscroll, paint_context);
}

static void
st_scroll_view_pick (ClutterActor       *actor,
                     ClutterPickContext *pick_context)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (actor)->priv;

  /* Chain up so we get a bounding box pained (if we are reactive) */
  CLUTTER_ACTOR_CLASS (st_scroll_view_parent_class)->pick (actor, pick_context);

  if (priv->child)
    clutter_actor_pick (priv->child, pick_context);
  if (priv->hscrollbar_visible)
    clutter_actor_pick (priv->hscroll, pick_context);
  if (priv->vscrollbar_visible)
    clutter_actor_pick (priv->vscroll, pick_context);
}

static gboolean
st_scroll_view_get_paint_volume (ClutterActor       *actor,
                                 ClutterPaintVolume *volume)
{
  return clutter_paint_volume_set_from_allocation (volume, actor);
}

static double
get_scrollbar_width (StScrollView *scroll,
                     gfloat        for_height)
{
  StScrollViewPrivate *priv = scroll->priv;

  if (clutter_actor_is_visible (priv->vscroll))
    {
      gfloat min_size;

      clutter_actor_get_preferred_width (CLUTTER_ACTOR (priv->vscroll), for_height,
                                         &min_size, NULL);
      return min_size;
    }
  else
    return 0;
}

static double
get_scrollbar_height (StScrollView *scroll,
                      gfloat        for_width)
{
  StScrollViewPrivate *priv = scroll->priv;

  if (clutter_actor_is_visible (priv->hscroll))
    {
      gfloat min_size;

      clutter_actor_get_preferred_height (CLUTTER_ACTOR (priv->hscroll), for_width,
                                          &min_size, NULL);

      return min_size;
    }
  else
    return 0;
}

static void
st_scroll_view_get_preferred_width (ClutterActor *actor,
                                    gfloat        for_height,
                                    gfloat       *min_width_p,
                                    gfloat       *natural_width_p)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (actor)->priv;
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  gboolean account_for_vscrollbar = FALSE;
  gfloat min_width = 0, natural_width;
  gfloat child_min_width, child_natural_width;

  if (!priv->child)
    return;

  st_theme_node_adjust_for_height (theme_node, &for_height);

  clutter_actor_get_preferred_width (priv->child, -1,
                                     &child_min_width, &child_natural_width);

  natural_width = child_natural_width;

  switch (priv->hscrollbar_policy)
    {
    case ST_POLICY_NEVER:
      min_width = child_min_width;
      break;
    case ST_POLICY_ALWAYS:
    case ST_POLICY_AUTOMATIC:
    case ST_POLICY_EXTERNAL:
      /* Should theoretically use the min width of the hscrollbar,
       * but that's not cleanly defined at the moment */
      min_width = 0;
      break;
    default:
      g_warn_if_reached();
      break;
    }

  switch (priv->vscrollbar_policy)
    {
    case ST_POLICY_NEVER:
    case ST_POLICY_EXTERNAL:
      account_for_vscrollbar = FALSE;
      break;
    case ST_POLICY_ALWAYS:
      account_for_vscrollbar = !priv->overlay_scrollbars;
      break;
    case ST_POLICY_AUTOMATIC:
      /* For automatic scrollbars, we always request space for the vertical
       * scrollbar; we won't know whether we actually need one until our
       * height is assigned in allocate().
       */
      account_for_vscrollbar = !priv->overlay_scrollbars;
      break;
    default:
      g_warn_if_reached();
      break;
    }

  if (account_for_vscrollbar)
    {
      float sb_width = get_scrollbar_width (ST_SCROLL_VIEW (actor), for_height);

      min_width += sb_width;
      natural_width += sb_width;
    }

  if (min_width_p)
    *min_width_p = min_width;

  if (natural_width_p)
    *natural_width_p = natural_width;

  st_theme_node_adjust_preferred_width (theme_node, min_width_p, natural_width_p);
}

static void
st_scroll_view_get_preferred_height (ClutterActor *actor,
                                     gfloat        for_width,
                                     gfloat       *min_height_p,
                                     gfloat       *natural_height_p)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (actor)->priv;
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  gboolean account_for_hscrollbar = FALSE;
  gfloat min_height = 0, natural_height;
  gfloat child_min_height, child_natural_height;
  gfloat child_min_width;
  gfloat sb_width;

  if (!priv->child)
    return;

  st_theme_node_adjust_for_width (theme_node, &for_width);

  clutter_actor_get_preferred_width (priv->child, -1,
                                     &child_min_width, NULL);

  if (min_height_p)
    *min_height_p = 0;

  sb_width = get_scrollbar_width (ST_SCROLL_VIEW (actor), -1);

  switch (priv->vscrollbar_policy)
    {
    case ST_POLICY_NEVER:
    case ST_POLICY_EXTERNAL:
      break;
    case ST_POLICY_ALWAYS:
    case ST_POLICY_AUTOMATIC:
      /* We've requested space for the scrollbar, subtract it back out */
      for_width -= sb_width;
      break;
    default:
      g_warn_if_reached();
      break;
    }

  switch (priv->hscrollbar_policy)
    {
    case ST_POLICY_NEVER:
    case ST_POLICY_EXTERNAL:
      account_for_hscrollbar = FALSE;
      break;
    case ST_POLICY_ALWAYS:
      account_for_hscrollbar = !priv->overlay_scrollbars;
      break;
    case ST_POLICY_AUTOMATIC:
      /* For automatic scrollbars, we always request space for the horizontal
       * scrollbar; we won't know whether we actually need one until our
       * width is assigned in allocate().
       */
      account_for_hscrollbar = !priv->overlay_scrollbars;
      break;
    default:
      g_warn_if_reached();
      break;
    }

  clutter_actor_get_preferred_height (priv->child, for_width,
                                      &child_min_height, &child_natural_height);

  natural_height = child_natural_height;

  switch (priv->vscrollbar_policy)
    {
    case ST_POLICY_NEVER:
      min_height = child_min_height;
      break;
    case ST_POLICY_ALWAYS:
    case ST_POLICY_AUTOMATIC:
    case ST_POLICY_EXTERNAL:
      /* Should theoretically use the min height of the vscrollbar,
       * but that's not cleanly defined at the moment */
      min_height = 0;
      break;
    default:
      g_warn_if_reached();
      break;
    }

  if (account_for_hscrollbar)
    {
      float sb_height = get_scrollbar_height (ST_SCROLL_VIEW (actor), for_width);

      min_height += sb_height;
      natural_height += sb_height;
    }

  if (min_height_p)
    *min_height_p = min_height;

  if (natural_height_p)
    *natural_height_p = natural_height;

  st_theme_node_adjust_preferred_height (theme_node, min_height_p, natural_height_p);
}

static void
st_scroll_view_allocate (ClutterActor          *actor,
                         const ClutterActorBox *box)
{
  ClutterActorBox content_box, child_box;
  gfloat avail_width, avail_height, sb_width, sb_height;
  gboolean hscrollbar_visible, vscrollbar_visible;

  StScrollViewPrivate *priv = ST_SCROLL_VIEW (actor)->priv;
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));

  clutter_actor_set_allocation (actor, box);

  st_theme_node_get_content_box (theme_node, box, &content_box);

  avail_width = content_box.x2 - content_box.x1;
  avail_height = content_box.y2 - content_box.y1;

  if (clutter_actor_get_request_mode (actor) == CLUTTER_REQUEST_HEIGHT_FOR_WIDTH)
    {
      sb_width = get_scrollbar_width (ST_SCROLL_VIEW (actor), -1);
      sb_height = get_scrollbar_height (ST_SCROLL_VIEW (actor), sb_width);
    }
  else
    {
      sb_height = get_scrollbar_height (ST_SCROLL_VIEW (actor), -1);
      sb_width = get_scrollbar_width (ST_SCROLL_VIEW (actor), sb_height);
    }

  /* Determine what scrollbars are visible. The basic idea of the
   * handling of an automatic scrollbars is that we start off with the
   * assumption that we don't need any scrollbars, see if that works,
   * and if not add horizontal and vertical scrollbars until we are no
   * longer overflowing.
   */
  if (priv->child)
    {
      gfloat child_min_width;
      gfloat child_min_height;

      clutter_actor_get_preferred_width (priv->child, -1,
                                         &child_min_width, NULL);

      if (priv->vscrollbar_policy == ST_POLICY_AUTOMATIC)
        {
          if (priv->hscrollbar_policy == ST_POLICY_AUTOMATIC)
            {
              /* Pass one, try without a vertical scrollbar */
              clutter_actor_get_preferred_height (priv->child, avail_width, &child_min_height, NULL);
              vscrollbar_visible = child_min_height > avail_height;
              hscrollbar_visible = child_min_width > avail_width - (vscrollbar_visible ? sb_width : 0);
              vscrollbar_visible = child_min_height > avail_height - (hscrollbar_visible ? sb_height : 0);

              /* Pass two - if we needed a vertical scrollbar, get a new preferred height */
              if (vscrollbar_visible)
                {
                  clutter_actor_get_preferred_height (priv->child, MAX (avail_width - sb_width, 0),
                                                      &child_min_height, NULL);
                  hscrollbar_visible = child_min_width > avail_width - sb_width;
                }
            }
          else
            {
              hscrollbar_visible = priv->hscrollbar_policy == ST_POLICY_ALWAYS;

              /* try without a vertical scrollbar */
              clutter_actor_get_preferred_height (priv->child, avail_width, &child_min_height, NULL);
              vscrollbar_visible = child_min_height > avail_height - (hscrollbar_visible ? sb_height : 0);
            }
        }
      else
        {
          vscrollbar_visible = priv->vscrollbar_policy == ST_POLICY_ALWAYS;

          if (priv->hscrollbar_policy == ST_POLICY_AUTOMATIC)
            hscrollbar_visible = child_min_width > avail_height - (vscrollbar_visible ? 0 : sb_width);
          else
            hscrollbar_visible = priv->hscrollbar_policy == ST_POLICY_ALWAYS;
        }
    }
  else
    {
      hscrollbar_visible = priv->hscrollbar_policy != ST_POLICY_NEVER &&
                           priv->hscrollbar_policy != ST_POLICY_EXTERNAL;
      vscrollbar_visible = priv->vscrollbar_policy != ST_POLICY_NEVER &&
                           priv->vscrollbar_policy != ST_POLICY_EXTERNAL;
    }

  /* Whether or not we show the scrollbars, if the scrollbars are visible
   * actors, we need to give them some allocation, so we unconditionally
   * give them the "right" allocation; that might overlap the child when
   * the scrollbars are not visible, but it doesn't matter because we
   * don't include them in pick or paint.
   */

  /* Vertical scrollbar */
  if (clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL)
    {
      child_box.x1 = content_box.x1;
      child_box.x2 = content_box.x1 + sb_width;
    }
  else
    {
      child_box.x1 = content_box.x2 - sb_width;
      child_box.x2 = content_box.x2;
    }
  child_box.y1 = content_box.y1;
  child_box.y2 = content_box.y2 - (hscrollbar_visible ? sb_height : 0);

  clutter_actor_allocate (priv->vscroll, &child_box);

  /* Horizontal scrollbar */
  if (clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL)
    {
      child_box.x1 = content_box.x1 + (vscrollbar_visible ? sb_width : 0);
      child_box.x2 = content_box.x2;
    }
  else
    {
      child_box.x1 = content_box.x1;
      child_box.x2 = content_box.x2 - (vscrollbar_visible ? sb_width : 0);
    }
  child_box.y1 = content_box.y2 - sb_height;
  child_box.y2 = content_box.y2;

  clutter_actor_allocate (priv->hscroll, &child_box);

  /* In case the scrollbar policy is NEVER or EXTERNAL or scrollbars
   * should be overlaid, we don't trim the content box allocation by
   * the scrollbar size.
   * Fold this into the scrollbar sizes to simplify the rest of the
   * computations.
   */
  if (priv->hscrollbar_policy == ST_POLICY_NEVER ||
      priv->hscrollbar_policy == ST_POLICY_EXTERNAL ||
      priv->overlay_scrollbars)
    sb_height = 0;
  if (priv->vscrollbar_policy == ST_POLICY_NEVER ||
      priv->vscrollbar_policy == ST_POLICY_EXTERNAL ||
      priv->overlay_scrollbars)
    sb_width = 0;

  /* Child */
  if (clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL)
    {
      child_box.x1 = content_box.x1 + sb_width;
      child_box.x2 = content_box.x2;
    }
  else
    {
      child_box.x1 = content_box.x1;
      child_box.x2 = content_box.x2 - sb_width;
    }
  child_box.y1 = content_box.y1;
  child_box.y2 = content_box.y2 - sb_height;

  if (priv->child)
    clutter_actor_allocate (priv->child, &child_box);

  if (priv->hscrollbar_visible != hscrollbar_visible)
    {
      g_object_freeze_notify (G_OBJECT (actor));
      priv->hscrollbar_visible = hscrollbar_visible;
      g_object_notify_by_pspec (G_OBJECT (actor),
                                props[PROP_HSCROLLBAR_VISIBLE]);
      g_object_thaw_notify (G_OBJECT (actor));
    }

  if (priv->vscrollbar_visible != vscrollbar_visible)
    {
      g_object_freeze_notify (G_OBJECT (actor));
      priv->vscrollbar_visible = vscrollbar_visible;
      g_object_notify_by_pspec (G_OBJECT (actor),
                                props[PROP_VSCROLLBAR_VISIBLE]);
      g_object_thaw_notify (G_OBJECT (actor));
    }

}

static void
adjust_with_direction (StAdjustment           *adj,
                       ClutterScrollDirection  direction)
{
  gdouble delta;

  switch (direction)
    {
    case CLUTTER_SCROLL_UP:
    case CLUTTER_SCROLL_LEFT:
      delta = -1.0;
      break;
    case CLUTTER_SCROLL_RIGHT:
    case CLUTTER_SCROLL_DOWN:
      delta = 1.0;
      break;
    case CLUTTER_SCROLL_SMOOTH:
    default:
      g_assert_not_reached ();
      break;
    }

  st_adjustment_adjust_for_scroll_event (adj, delta);
}

static void
st_scroll_view_style_changed (StWidget *widget)
{
  StScrollView *self = ST_SCROLL_VIEW (widget);
  gboolean has_vfade, has_hfade;
  double vfade_offset = 0.0;
  double hfade_offset = 0.0;

  StThemeNode *theme_node = st_widget_get_theme_node (widget);

  has_vfade = st_theme_node_lookup_length (theme_node, "-st-vfade-offset", FALSE, &vfade_offset);
  has_hfade = st_theme_node_lookup_length (theme_node, "-st-hfade-offset", FALSE, &hfade_offset);
  if (has_vfade || has_hfade)
    {
      st_scroll_view_update_fade_effect (self,
                                         &(ClutterMargin) {
                                           .top = vfade_offset,
                                           .bottom = vfade_offset,
                                           .left = hfade_offset,
                                           .right = hfade_offset,
                                         });
    }

  ST_WIDGET_CLASS (st_scroll_view_parent_class)->style_changed (widget);
}

static gboolean
st_scroll_view_scroll_event (ClutterActor *self,
                             ClutterEvent *event)
{
  StScrollViewPrivate *priv = ST_SCROLL_VIEW (self)->priv;
  ClutterTextDirection direction;
  ClutterScrollDirection scroll_direction;

  /* don't handle scroll events if requested not to */
  if (!priv->mouse_scroll)
    return FALSE;

  if (!!(clutter_event_get_flags (event) &
         CLUTTER_EVENT_FLAG_POINTER_EMULATED))
    return TRUE;

  direction = clutter_actor_get_text_direction (self);
  scroll_direction = clutter_event_get_scroll_direction (event);

  switch (scroll_direction)
    {
    case CLUTTER_SCROLL_SMOOTH:
      {
        gdouble delta_x, delta_y;
        clutter_event_get_scroll_delta (event, &delta_x, &delta_y);

        if (direction == CLUTTER_TEXT_DIRECTION_RTL)
          delta_x *= -1;

        st_adjustment_adjust_for_scroll_event (priv->hadjustment, delta_x);
        st_adjustment_adjust_for_scroll_event (priv->vadjustment, delta_y);
      }
      break;
    case CLUTTER_SCROLL_UP:
    case CLUTTER_SCROLL_DOWN:
      adjust_with_direction (priv->vadjustment, scroll_direction);
      break;
    case CLUTTER_SCROLL_LEFT:
    case CLUTTER_SCROLL_RIGHT:
      if (direction == CLUTTER_TEXT_DIRECTION_RTL)
        {
          ClutterScrollDirection dir;

          dir = scroll_direction == CLUTTER_SCROLL_LEFT ? CLUTTER_SCROLL_RIGHT
                                                        : CLUTTER_SCROLL_LEFT;
          adjust_with_direction (priv->hadjustment, dir);
        }
      else
        {
          adjust_with_direction (priv->hadjustment, scroll_direction);
        }
      break;
    default:
      g_warn_if_reached();
      break;
    }

  return TRUE;
}

static void
st_scroll_view_class_init (StScrollViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->get_property = st_scroll_view_get_property;
  object_class->set_property = st_scroll_view_set_property;
  object_class->dispose = st_scroll_view_dispose;

  actor_class->paint = st_scroll_view_paint;
  actor_class->pick = st_scroll_view_pick;
  actor_class->get_paint_volume = st_scroll_view_get_paint_volume;
  actor_class->get_preferred_width = st_scroll_view_get_preferred_width;
  actor_class->get_preferred_height = st_scroll_view_get_preferred_height;
  actor_class->allocate = st_scroll_view_allocate;
  actor_class->scroll_event = st_scroll_view_scroll_event;

  widget_class->style_changed = st_scroll_view_style_changed;

  /**
   * StScrollView:hscroll:
   *
   * The horizontal #StScrollBar for the #StScrollView.
   */
  props[PROP_HSCROLL] =
    g_param_spec_object ("hscroll",
                         "StScrollBar",
                         "Horizontal scroll indicator",
                         ST_TYPE_SCROLL_BAR,
                         ST_PARAM_READABLE);

  /**
   * StScrollView:vscroll:
   *
   * The vertical #StScrollBar for the #StScrollView.
   */
  props[PROP_VSCROLL] =
    g_param_spec_object ("vscroll",
                         "StScrollBar",
                         "Vertical scroll indicator",
                         ST_TYPE_SCROLL_BAR,
                         ST_PARAM_READABLE);

  /**
   * StScrollView:vscrollbar-policy:
   *
   * The #StPolicyType for when to show the vertical #StScrollBar.
   */
  props[PROP_VSCROLLBAR_POLICY] =
    g_param_spec_enum ("vscrollbar-policy",
                       "Vertical Scrollbar Policy",
                       "When the vertical scrollbar is displayed",
                       ST_TYPE_POLICY_TYPE,
                       ST_POLICY_AUTOMATIC,
                       ST_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * StScrollView:hscrollbar-policy:
   *
   * The #StPolicyType for when to show the horizontal #StScrollBar.
   */
  props[PROP_HSCROLLBAR_POLICY] =
    g_param_spec_enum ("hscrollbar-policy",
                       "Horizontal Scrollbar Policy",
                       "When the horizontal scrollbar is displayed",
                       ST_TYPE_POLICY_TYPE,
                       ST_POLICY_AUTOMATIC,
                       ST_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * StScrollView:hscrollbar-visible:
   *
   * Whether the horizontal #StScrollBar is visible.
   */
  props[PROP_HSCROLLBAR_VISIBLE] =
    g_param_spec_boolean ("hscrollbar-visible",
                          "Horizontal Scrollbar Visibility",
                          "Whether the horizontal scrollbar is visible",
                          TRUE,
                          ST_PARAM_READABLE);

  /**
   * StScrollView:vscrollbar-visible:
   *
   * Whether the vertical #StScrollBar is visible.
   */
  props[PROP_VSCROLLBAR_VISIBLE] =
    g_param_spec_boolean ("vscrollbar-visible",
                          "Vertical Scrollbar Visibility",
                          "Whether the vertical scrollbar is visible",
                          TRUE,
                          ST_PARAM_READABLE);

  /**
   * StScrollView:enable-mouse-scrolling:
   *
   * Whether to enable automatic mouse wheel scrolling.
   */
  props[PROP_MOUSE_SCROLL] =
    g_param_spec_boolean ("enable-mouse-scrolling",
                          "Enable Mouse Scrolling",
                          "Enable automatic mouse wheel scrolling",
                          TRUE,
                          ST_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * StScrollView:overlay-scrollbars:
   *
   * Whether scrollbars are painted on top of the content.
   */
  props[PROP_OVERLAY_SCROLLBARS] =
    g_param_spec_boolean ("overlay-scrollbars",
                          "Use Overlay Scrollbars",
                          "Overlay scrollbars over the content",
                          FALSE,
                          ST_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
st_scroll_view_init (StScrollView *self)
{
  StScrollViewPrivate *priv = self->priv = st_scroll_view_get_instance_private (self);

  priv->hscrollbar_policy = ST_POLICY_AUTOMATIC;
  priv->vscrollbar_policy = ST_POLICY_AUTOMATIC;

  priv->hadjustment = g_object_new (ST_TYPE_ADJUSTMENT,
                                    "actor", self,
                                    NULL);
  priv->hscroll = g_object_new (ST_TYPE_SCROLL_BAR,
                                "adjustment", priv->hadjustment,
                                "vertical", FALSE,
                                NULL);

  priv->vadjustment = g_object_new (ST_TYPE_ADJUSTMENT,
                                    "actor", self,
                                    NULL);
  priv->vscroll = g_object_new (ST_TYPE_SCROLL_BAR,
                                "adjustment", priv->vadjustment,
                                "vertical", TRUE,
                                NULL);

  clutter_actor_add_child (CLUTTER_ACTOR (self), priv->hscroll);
  clutter_actor_add_child (CLUTTER_ACTOR (self), priv->vscroll);

  /* mouse scroll is enabled by default, so we also need to be reactive */
  priv->mouse_scroll = TRUE;
  g_object_set (G_OBJECT (self), "reactive", TRUE, NULL);
}

static void
st_scroll_view_add (ClutterContainer *container,
                    ClutterActor     *actor)
{
  StScrollView *self = ST_SCROLL_VIEW (container);
  StScrollViewPrivate *priv = self->priv;

  if (ST_IS_SCROLLABLE (actor))
    {
      priv->child = actor;

      /* chain up to StBin::add() */
      st_scroll_view_parent_iface->add (container, actor);

      st_scrollable_set_adjustments (ST_SCROLLABLE (actor),
                                     priv->hadjustment, priv->vadjustment);
    }
  else
    {
      g_warning ("Attempting to add an actor of type %s to "
                 "a StScrollView, but the actor does "
                 "not implement StScrollable.",
                 g_type_name (G_OBJECT_TYPE (actor)));
    }
}

static void
st_scroll_view_remove (ClutterContainer *container,
                       ClutterActor     *actor)
{
  StScrollView *self = ST_SCROLL_VIEW (container);
  StScrollViewPrivate *priv = self->priv;

  if (actor == priv->child)
    {
      g_object_ref (priv->child);

      /* chain up to StBin::remove() */
      st_scroll_view_parent_iface->remove (container, actor);

      st_scrollable_set_adjustments (ST_SCROLLABLE (priv->child),
                                     NULL, NULL);

      g_object_unref (priv->child);
      priv->child = NULL;
    }
  else
    {
      if (actor == priv->vscroll)
        priv->vscroll = NULL;
      else if (actor == priv->hscroll)
        priv->hscroll = NULL;
      else
        g_assert ("Unknown child removed from StScrollView");

      clutter_actor_remove_child (CLUTTER_ACTOR (container), actor);
    }
}

static void
clutter_container_iface_init (ClutterContainerIface *iface)
{
  /* store a pointer to the StBin implementation of
   * ClutterContainer so that we can chain up when
   * overriding the methods
   */
  st_scroll_view_parent_iface = g_type_interface_peek_parent (iface);

  iface->add = st_scroll_view_add;
  iface->remove = st_scroll_view_remove;
}

/**
 * st_scroll_view_new:
 *
 * Create a new #StScrollView.
 *
 * Returns: (transfer full): a new #StScrollView
 */
StWidget *
st_scroll_view_new (void)
{
  return g_object_new (ST_TYPE_SCROLL_VIEW, NULL);
}

/**
 * st_scroll_view_get_hscroll_bar:
 * @scroll: a #StScrollView
 *
 * Gets the horizontal #StScrollBar of the #StScrollView.
 *
 * Returns: (transfer none): the horizontal scrollbar
 */
ClutterActor *
st_scroll_view_get_hscroll_bar (StScrollView *scroll)
{
  g_return_val_if_fail (ST_IS_SCROLL_VIEW (scroll), NULL);

  return scroll->priv->hscroll;
}

/**
 * st_scroll_view_get_vscroll_bar:
 * @scroll: a #StScrollView
 *
 * Gets the vertical scrollbar of the #StScrollView.
 *
 * Returns: (transfer none): the vertical #StScrollBar
 */
ClutterActor *
st_scroll_view_get_vscroll_bar (StScrollView *scroll)
{
  g_return_val_if_fail (ST_IS_SCROLL_VIEW (scroll), NULL);

  return scroll->priv->vscroll;
}

/**
 * st_scroll_view_get_column_size:
 * @scroll: a #StScrollView
 *
 * Get the step increment of the horizontal plane.
 *
 * Returns: the horizontal step increment
 */
gfloat
st_scroll_view_get_column_size (StScrollView *scroll)
{
  gdouble column_size;

  g_return_val_if_fail (scroll, 0);

  g_object_get (scroll->priv->hadjustment,
                "step-increment", &column_size,
                NULL);

  return column_size;
}

/**
 * st_scroll_view_set_column_size:
 * @scroll: a #StScrollView
 * @column_size: horizontal step increment
 *
 * Set the step increment of the horizontal plane to @column_size.
 */
void
st_scroll_view_set_column_size (StScrollView *scroll,
                                gfloat        column_size)
{
  g_return_if_fail (scroll);

  if (column_size < 0)
    {
      scroll->priv->column_size_set = FALSE;
      scroll->priv->column_size = -1;
    }
  else
    {
      scroll->priv->column_size_set = TRUE;
      scroll->priv->column_size = column_size;

      g_object_set (scroll->priv->hadjustment,
                    "step-increment", (gdouble) scroll->priv->column_size,
                    NULL);
    }
}

/**
 * st_scroll_view_get_row_size:
 * @scroll: a #StScrollView
 *
 * Get the step increment of the vertical plane.
 *
 * Returns: the vertical step increment
 */
gfloat
st_scroll_view_get_row_size (StScrollView *scroll)
{
  gdouble row_size;

  g_return_val_if_fail (scroll, 0);

  g_object_get (scroll->priv->vadjustment,
                "step-increment", &row_size,
                NULL);

  return row_size;
}

/**
 * st_scroll_view_set_row_size:
 * @scroll: a #StScrollView
 * @row_size: vertical step increment
 *
 * Set the step increment of the vertical plane to @row_size.
 */
void
st_scroll_view_set_row_size (StScrollView *scroll,
                             gfloat        row_size)
{
  g_return_if_fail (scroll);

  if (row_size < 0)
    {
      scroll->priv->row_size_set = FALSE;
      scroll->priv->row_size = -1;
    }
  else
    {
      scroll->priv->row_size_set = TRUE;
      scroll->priv->row_size = row_size;

      g_object_set (scroll->priv->vadjustment,
                    "step-increment", (gdouble) scroll->priv->row_size,
                    NULL);
    }
}

/**
 * st_scroll_view_set_mouse_scrolling:
 * @scroll: a #StScrollView
 * @enabled: %TRUE or %FALSE
 *
 * Sets automatic mouse wheel scrolling to enabled or disabled.
 */
void
st_scroll_view_set_mouse_scrolling (StScrollView *scroll,
                                    gboolean      enabled)
{
  StScrollViewPrivate *priv;

  g_return_if_fail (ST_IS_SCROLL_VIEW (scroll));

  priv = ST_SCROLL_VIEW (scroll)->priv;

  if (priv->mouse_scroll != enabled)
    {
      priv->mouse_scroll = enabled;

      /* make sure we can receive mouse wheel events */
      if (enabled)
        clutter_actor_set_reactive ((ClutterActor *) scroll, TRUE);

      g_object_notify_by_pspec (G_OBJECT (scroll), props[PROP_MOUSE_SCROLL]);
    }
}

/**
 * st_scroll_view_get_mouse_scrolling:
 * @scroll: a #StScrollView
 *
 * Get whether automatic mouse wheel scrolling is enabled or disabled.
 *
 * Returns: %TRUE if enabled, %FALSE otherwise
 */
gboolean
st_scroll_view_get_mouse_scrolling (StScrollView *scroll)
{
  StScrollViewPrivate *priv;

  g_return_val_if_fail (ST_IS_SCROLL_VIEW (scroll), FALSE);

  priv = ST_SCROLL_VIEW (scroll)->priv;

  return priv->mouse_scroll;
}

/**
 * st_scroll_view_set_overlay_scrollbars:
 * @scroll: A #StScrollView
 * @enabled: Whether to enable overlay scrollbars
 *
 * Sets whether scrollbars are painted on top of the content.
 */
void
st_scroll_view_set_overlay_scrollbars (StScrollView *scroll,
                                       gboolean      enabled)
{
  StScrollViewPrivate *priv;

  g_return_if_fail (ST_IS_SCROLL_VIEW (scroll));

  priv = ST_SCROLL_VIEW (scroll)->priv;

  if (priv->overlay_scrollbars != enabled)
    {
      priv->overlay_scrollbars = enabled;
      g_object_notify_by_pspec (G_OBJECT (scroll),
                                props[PROP_OVERLAY_SCROLLBARS]);
      clutter_actor_queue_relayout (CLUTTER_ACTOR (scroll));
    }
}

/**
 * st_scroll_view_get_overlay_scrollbars:
 * @scroll: A #StScrollView
 *
 * Gets whether scrollbars are painted on top of the content.
 *
 * Returns: %TRUE if enabled, %FALSE otherwise
 */
gboolean
st_scroll_view_get_overlay_scrollbars (StScrollView *scroll)
{
  StScrollViewPrivate *priv;

  g_return_val_if_fail (ST_IS_SCROLL_VIEW (scroll), FALSE);

  priv = ST_SCROLL_VIEW (scroll)->priv;

  return priv->overlay_scrollbars;
}

/**
 * st_scroll_view_set_policy:
 * @scroll: A #StScrollView
 * @hscroll: Whether to enable horizontal scrolling
 * @vscroll: Whether to enable vertical scrolling
 *
 * Set the scroll policy.
 */
void
st_scroll_view_set_policy (StScrollView   *scroll,
                           StPolicyType    hscroll,
                           StPolicyType    vscroll)
{
  StScrollViewPrivate *priv;

  g_return_if_fail (ST_IS_SCROLL_VIEW (scroll));

  priv = ST_SCROLL_VIEW (scroll)->priv;

  if (priv->hscrollbar_policy == hscroll && priv->vscrollbar_policy == vscroll)
    return;

  g_object_freeze_notify ((GObject *) scroll);

  if (priv->hscrollbar_policy != hscroll)
    {
      priv->hscrollbar_policy = hscroll;
      g_object_notify_by_pspec ((GObject *) scroll,
                                props[PROP_HSCROLLBAR_POLICY]);
    }

  if (priv->vscrollbar_policy != vscroll)
    {
      priv->vscrollbar_policy = vscroll;
      g_object_notify_by_pspec ((GObject *) scroll,
                                props[PROP_VSCROLLBAR_POLICY]);
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (scroll));

  g_object_thaw_notify ((GObject *) scroll);
}
