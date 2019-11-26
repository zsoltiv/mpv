#ifndef MP_WIN_STATE_H_
#define MP_WIN_STATE_H_

#include "common/common.h"

struct vo;

enum {
    // By user settings, the window manager's chosen window position should
    // be overridden.
    VO_WIN_FORCE_POS = (1 << 0),
};

struct vo_win_geometry {
    // Bitfield of VO_WIN_* flags
    int flags;
    // Position & size of the window. In xinerama coordinates, i.e. they're
    // relative to the virtual desktop encompassing all screens, not the
    // current screen.
    struct mp_rect win;
    // Aspect ratio of the current monitor.
    // (calculated from screen size and options.)
    double monitor_par;
};

void vo_calc_window_geometry(struct vo *vo, const struct mp_rect *screen,
                             struct vo_win_geometry *out_geo);
void vo_calc_window_geometry2(struct vo *vo, const struct mp_rect *screen,
                              double dpi_scale, struct vo_win_geometry *out_geo);
void vo_apply_window_geometry(struct vo *vo, const struct vo_win_geometry *geo);

// Currently manages some user options.
struct vo_win_state;

enum vo_win_states {
    VO_WIN_STATE_FULLSCREEN,    // bool
    VO_WIN_STATE_MINIMIZE,      // bool
    VO_WIN_STATE_MAXIMIZE,      // bool
    VO_WIN_STATE_ON_TOP,        // bool
    VO_WIN_STATE_BORDER,        // bool
    VO_WIN_STATE_ALL_WS,        // bool

    VO_WIN_STATE_COUNT
};

// Destroy with talloc_free().
// Note: this must be strictly destroyed before vo.
// This hooks itself into vo (in a thread-safe way), and you can have only one
// per vo.
struct vo_win_state *vo_win_state_create(struct vo *vo);

// Note: it's _not_ OK to use vo->opts instead (e.g. vo->opts->fullscreen
//       instead of this opt struct. This is because vo->opts is managed for the
//       VO thread, so this breaks with backends that do windowing on a foreign
//       thread. You may also use vo_get_win_opts().
struct mp_vo_opts *vo_win_state_opts(struct vo_win_state *st);

// Update state in reaction to other events. Normally, you want to call this
// when receiving VOCTRL_VO_STATE_UPDATE.
// This returns a bit-field of externally changed event, using vo_win_states as
// bit position. E.g. if fullscreen and on-top changes, this would return
// (1 << VO_WIN_STATE_FULLSCREEN) | (1 << VO_WIN_STATE_ON_TOP).
uint64_t vo_win_state_update(struct vo_win_state *st);

// Query the current user-desired state (basically, return the option value).
// This is equivalent to using vo_win_state_opts()->[field mapping to state].
bool vo_win_state_get_bool(struct vo_win_state *st, enum vo_win_states state);

// Update the current state, usually in reaction to external events.
// This is equivalent to setting vo_win_state_opts()->[field mapping to state]
// to the new value, and calling vo_win_state_mark_external_changed().
void vo_win_state_report_bool(struct vo_win_state *st, enum vo_win_states state,
                              bool val);

// Notify that the current state was updated, usually in reaction to external
// events. "field" must be a pointer to a field in vo_win_state_opts() (that
// is an option, and a direct member of struct mp_subtitle_opts).
void vo_win_state_report_external_changed(struct vo_win_state *st, void *field);

// Internal: get and reset next changed state (option-managed fields only).
// Returns state value if it was externally changed, or -1 if not.
// If state<0, then get next changed state, otherwise use fixed state.
int vo_win_state_fetch_ext(struct vo_win_state *st, int state,
                           union m_option_value *val);

// Internal, a hack.
char *vo_win_state_opt(enum vo_win_states state);

#endif
