/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include "common/common.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "win_state.h"
#include "vo.h"

#include "video/mp_image.h"

static void calc_monitor_aspect(struct mp_vo_opts *opts, int scr_w, int scr_h,
                                double *pixelaspect, int *w, int *h)
{
    *pixelaspect = 1.0 / opts->monitor_pixel_aspect;

    if (scr_w > 0 && scr_h > 0 && opts->force_monitor_aspect)
        *pixelaspect = 1.0 / (opts->force_monitor_aspect * scr_h / scr_w);

    if (*pixelaspect < 1) {
        *h /= *pixelaspect;
    } else {
        *w *= *pixelaspect;
    }
}

// Fit *w/*h into the size specified by geo.
static void apply_autofit(int *w, int *h, int scr_w, int scr_h,
                          struct m_geometry *geo, bool allow_up, bool allow_down)
{
    if (!geo->wh_valid)
        return;

    int dummy = 0;
    int n_w = *w, n_h = *h;
    m_geometry_apply(&dummy, &dummy, &n_w, &n_h, scr_w, scr_h, geo);

    if (!allow_up && *w <= n_w && *h <= n_h)
        return;
    if (!allow_down && *w >= n_w && *h >= n_h)
        return;

    // If aspect mismatches, always make the window smaller than the fit box
    // (Or larger, if allow_down==false.)
    double asp = (double)*w / *h;
    double n_asp = (double)n_w / n_h;
    if ((n_asp <= asp) == allow_down) {
        *w = n_w;
        *h = n_w / asp;
    } else {
        *w = n_h * asp;
        *h = n_h;
    }
}

// Compute the "suggested" window size and position and return it in *out_geo.
// screen is the bounding box of the current screen within the virtual desktop.
// Does not change *vo.
//  screen: position of the screen on virtual desktop on which the window
//          should be placed
//  dpi_scale: the DPI multiplier to get from virtual to real coordinates
//             (>1 for "hidpi")
// Use vo_apply_window_geometry() to copy the result into the vo.
// NOTE: currently, all windowing backends do their own handling of window
//       geometry additional to this code. This is to deal with initial window
//       placement, fullscreen handling, avoiding resize on reconfig() with no
//       size change, multi-monitor stuff, and possibly more.
void vo_calc_window_geometry2(struct vo *vo, const struct mp_rect *screen,
                              double dpi_scale, struct vo_win_geometry *out_geo)
{
    struct mp_vo_opts *opts = vo->opts;

    *out_geo = (struct vo_win_geometry){0};

    // The case of calling this function even though no video was configured
    // yet (i.e. vo->params==NULL) happens when vo_gpu creates a hidden window
    // in order to create a rendering context.
    struct mp_image_params params = { .w = 320, .h = 200 };
    if (vo->params)
        params = *vo->params;

    if (!opts->hidpi_window_scale)
        dpi_scale = 1;

    int d_w, d_h;
    mp_image_params_get_dsize(&params, &d_w, &d_h);
    if ((vo->driver->caps & VO_CAP_ROTATE90) && params.rotate % 180 == 90)
        MPSWAP(int, d_w, d_h);
    d_w = MPCLAMP(d_w * opts->window_scale * dpi_scale, 1, 16000);
    d_h = MPCLAMP(d_h * opts->window_scale * dpi_scale, 1, 16000);

    int scr_w = screen->x1 - screen->x0;
    int scr_h = screen->y1 - screen->y0;

    MP_DBG(vo, "screen size: %dx%d\n", scr_w, scr_h);

    calc_monitor_aspect(opts, scr_w, scr_h, &out_geo->monitor_par, &d_w, &d_h);

    apply_autofit(&d_w, &d_h, scr_w, scr_h, &opts->autofit, true, true);
    apply_autofit(&d_w, &d_h, scr_w, scr_h, &opts->autofit_smaller, true, false);
    apply_autofit(&d_w, &d_h, scr_w, scr_h, &opts->autofit_larger, false, true);

    out_geo->win.x0 = (int)(scr_w - d_w) / 2;
    out_geo->win.y0 = (int)(scr_h - d_h) / 2;
    m_geometry_apply(&out_geo->win.x0, &out_geo->win.y0, &d_w, &d_h,
                     scr_w, scr_h, &opts->geometry);

    out_geo->win.x0 += screen->x0;
    out_geo->win.y0 += screen->y0;
    out_geo->win.x1 = out_geo->win.x0 + d_w;
    out_geo->win.y1 = out_geo->win.y0 + d_h;

    if (opts->geometry.xy_valid || opts->force_window_position)
        out_geo->flags |= VO_WIN_FORCE_POS;
}

void vo_calc_window_geometry(struct vo *vo, const struct mp_rect *screen,
                             struct vo_win_geometry *out_geo)
{
    vo_calc_window_geometry2(vo, screen, 1.0, out_geo);
}

// Copy the parameters in *geo to the vo fields.
// (Doesn't do anything else - windowing backends should trigger VO_EVENT_RESIZE
//  to ensure that the VO reinitializes rendering properly.)
void vo_apply_window_geometry(struct vo *vo, const struct vo_win_geometry *geo)
{
    vo->dwidth = geo->win.x1 - geo->win.x0;
    vo->dheight = geo->win.y1 - geo->win.y0;
    vo->monitor_par = geo->monitor_par;
}

struct vo_win_state {
    pthread_mutex_t lock;

    struct vo *vo;

    struct m_config_cache *opts_cache;
    struct mp_vo_opts *opts;

    struct m_option types[VO_WIN_STATE_COUNT];
    void *opt_map[VO_WIN_STATE_COUNT];
    uint64_t external_changed; // VO_WIN_STATE_* bit field
    // If external_changed bit set, this is the external "fixed" value.
    // Otherwise, this is the current/previous value.
    union m_option_value fixed[VO_WIN_STATE_COUNT];

    // These are not options.
    int minimize, maximize;
};

static void win_state_destroy(void *p)
{
    struct vo_win_state *st = p;

    vo_set_internal_win_state(st->vo, NULL);

    pthread_mutex_destroy(&st->lock);
}

struct vo_win_state *vo_win_state_create(struct vo *vo)
{
    struct vo_win_state *st = talloc_zero(NULL, struct vo_win_state);
    talloc_set_destructor(st, win_state_destroy);

    pthread_mutex_init(&st->lock, NULL);

    st->vo = vo;

    st->opts_cache = m_config_cache_alloc(st, vo->global, &vo_sub_opts);
    st->opts = st->opts_cache->opts;

    st->opt_map[VO_WIN_STATE_FULLSCREEN] = &st->opts->fullscreen;
    st->opt_map[VO_WIN_STATE_MINIMIZE] = &st->minimize;
    st->opt_map[VO_WIN_STATE_MAXIMIZE] = &st->maximize;
    st->opt_map[VO_WIN_STATE_ON_TOP] = &st->opts->ontop;
    st->opt_map[VO_WIN_STATE_BORDER] = &st->opts->border;
    st->opt_map[VO_WIN_STATE_ALL_WS] = &st->opts->all_workspaces;

    for (int n = 0; n < MP_ARRAY_SIZE(st->opt_map); n++) {
        // Currently all the same.
        st->types[n] = (struct m_option){
            .type = &m_option_type_flag,
        };

        // Copy initial value.
        m_option_copy(&st->types[n], &st->fixed[n], st->opt_map[n]);
    }

    vo_set_internal_win_state(vo, st);
    return st;
}

char *vo_win_state_opt(enum vo_win_states state)
{
    switch (state) {
    case VO_WIN_STATE_FULLSCREEN:   return "fullscreen";
    case VO_WIN_STATE_ON_TOP:       return "ontop";
    case VO_WIN_STATE_BORDER:       return "border";
    case VO_WIN_STATE_ALL_WS:       return "on-all-workspaces";
    default: return NULL; // not managed as option
    }
}

struct mp_vo_opts *vo_win_state_opts(struct vo_win_state *st)
{
    return st->opts;
}

// (The generic option code does not have this because it's too complex to
// support for _all_ option types.)
static bool opt_equals(struct m_option *t, void *v1, void *v2)
{
    if (t->type == &m_option_type_flag) {
        return *(int *)v1 == *(int *)v2;
    } else {
        assert(0); // well, add it
    }
}

uint64_t vo_win_state_update(struct vo_win_state *st)
{
    uint64_t changed = 0;

    pthread_mutex_lock(&st->lock);

    if (m_config_cache_update(st->opts_cache)) {
        // Ignore changes to any "fixed" fields, but return other changed fields.
        for (int n = 0; n < MP_ARRAY_SIZE(st->opt_map); n++) {
            if (st->external_changed & (1ull << n))
                m_option_copy(&st->types[n], st->opt_map[n], &st->fixed[n]);

            if (!opt_equals(&st->types[n], st->opt_map[n], &st->fixed[n])) {
                changed |= (1ull << n);

                m_option_copy(&st->types[n], &st->fixed[n], st->opt_map[n]);
            }
        }
    }

    pthread_mutex_unlock(&st->lock);

    return changed;
}

bool vo_win_state_get_bool(struct vo_win_state *st, enum vo_win_states state)
{
    assert(st->types[state].type == &m_option_type_flag);
    return *(int *)st->opt_map[state];
}

void vo_win_state_report_bool(struct vo_win_state *st, enum vo_win_states state,
                              bool val)
{
    assert(st->types[state].type == &m_option_type_flag);
    *(int *)st->opt_map[state] = val;
    vo_win_state_report_external_changed(st, st->opt_map[state]);
}

void vo_win_state_report_external_changed(struct vo_win_state *st, void *field)
{
    int state = -1;
    for (int n = 0; n < MP_ARRAY_SIZE(st->opt_map); n++) {
        if (st->opt_map[n] == field) {
            state = n;
            break;
        }
    }

    assert(state >= 0); // user passed non-managed field, or uses wrong opt. struct

    // "Fix" the option to avoid that concurrent or recursive option updates
    // clobber it (urgh).
    pthread_mutex_lock(&st->lock);
    st->external_changed |= 1ull << state;
    m_option_copy(&st->types[state], &st->fixed[state], field);
    pthread_mutex_unlock(&st->lock);

    // Causes some magic code to call vo_win_state_fetch_ext() to reset the
    // fixed option.
    vo_event(st->vo, VO_EVENT_WIN_STATE2);
}

int vo_win_state_fetch_ext(struct vo_win_state *st, int state,
                           union m_option_value *val)
{
    pthread_mutex_lock(&st->lock);

    if (state < 0) {
        for (int n = 0; n < MP_ARRAY_SIZE(st->opt_map); n++) {
            uint64_t mask = 1ull << n;
            if (st->external_changed & mask) {
                st->external_changed &= ~mask;
                state = n;
                break;
            }
        }
    }

    if (state >= 0)
        m_option_copy(&st->types[state], val, st->opt_map[state]);

    pthread_mutex_unlock(&st->lock);

    return state;
}
