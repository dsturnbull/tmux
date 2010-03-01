/* $Id: cmd-select-pane.c,v 1.10 2009/07/30 20:45:20 tcunha Exp $ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include "tmux.h"

#define WP_L 0x01
#define WP_R 0x02
#define WP_U 0x04
#define WP_D 0x08

/*
 * Select pane.
 */

int	cmd_select_pane_exec(struct cmd *, struct cmd_ctx *);
ARRAY_DECL(panes, struct window_pane *) panes;
int wp_compare(const void *a, const void *v, bool xoff, bool sign);
struct window_pane *wp_next(struct window_pane *wp, int dir,
		bool (*file)(struct window_pane *, struct window_pane *),
		int (*sort)(const void *a, const void *b));
bool wp_next_l_filt(struct window_pane *our, struct window_pane *their);
bool wp_next_r_filt(struct window_pane *our, struct window_pane *their);
bool wp_next_u_filt(struct window_pane *our, struct window_pane *their);
bool wp_next_d_filt(struct window_pane *our, struct window_pane *their);
int wp_next_l_sort(const void *a, const void *b);
int wp_next_r_sort(const void *a, const void *b);
int wp_next_u_sort(const void *a, const void *b);
int wp_next_d_sort(const void *a, const void *b);

const struct cmd_entry cmd_select_pane_entry = {
	"select-pane", "selectp",
	"[-LRDU] " CMD_TARGET_PANE_USAGE " [adjustment]",
	CMD_ARG01,
	CMD_CHFLAG('D')|CMD_CHFLAG('L')|CMD_CHFLAG('R')|CMD_CHFLAG('U'),
	cmd_target_init,
	cmd_target_parse,
	cmd_select_pane_exec,
	cmd_target_free,
	cmd_target_print
};

// cmd_select_pane_exec
int
cmd_select_pane_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct	window			*w;
	struct	winlink			*wl;
	struct	window_pane		*wp;
	struct	window_pane		*tar_wp;
	struct	cmd_target_data	*data = self->data;
	int		dir;
	bool	(*filt)(struct window_pane *our, struct window_pane *their);
	int		(*sort)(const void *a, const void *b);

	if ((wl = cmd_find_pane(ctx, data->target, NULL, &wp)) == NULL)
		return (-1);
	w = wl->window;

	if (!window_pane_visible(wp)) {
		ctx->error(ctx, "pane not visible: %s", data->target);
		return (-1);
	}

	if (!data->chflags) {
		window_set_active_pane(wl->window, wp);
		return (0);
	}

	// directions
	if (data->chflags & (CMD_CHFLAG('L'))) {
		filt	= wp_next_l_filt;
		sort	= wp_next_l_sort;
		dir		= WP_L;
	}
	if (data->chflags & (CMD_CHFLAG('R'))) {
		filt	= wp_next_r_filt;
		sort	= wp_next_r_sort;
		dir=	WP_R;
	}
	if (data->chflags & (CMD_CHFLAG('U'))) {
		filt	= wp_next_u_filt;
		sort	= wp_next_u_sort;
		dir		= WP_U;
	}
	if (data->chflags & (CMD_CHFLAG('D'))) {
		filt	= wp_next_d_filt;
		sort	= wp_next_d_sort;
		dir		= WP_D;
	}

	if (filt && sort)
		if ((tar_wp = wp_next(wp, dir, filt, sort)) != NULL) {
			window_set_active_pane(wl->window, tar_wp);
			server_status_window(wl->window);
		}

	return (0);
}

// wp_next
struct
window_pane *wp_next(
	struct window_pane *cur_wp, int dir,
	bool (*filt)(struct window_pane *, struct window_pane *),
	int (*sort)(const void *a, const void *b))
{
	struct window_pane *wp;
	struct window_pane *tar_wp;

	ARRAY_INIT(&panes);
	TAILQ_FOREACH(wp, &cur_wp->window->panes, entry)
		if (filt(cur_wp, wp))
			ARRAY_ADD(&panes, wp);

	// bypass filter
	if (ARRAY_LENGTH(&panes) == 0)
		TAILQ_FOREACH(wp, &cur_wp->window->panes, entry)
			switch(dir) {
				case WP_L:
					if (wp->xoff < cur_wp->xoff) ARRAY_ADD(&panes, wp);
					break;
				case WP_R:
					if (wp->xoff > cur_wp->xoff) ARRAY_ADD(&panes, wp);
					break;
				case WP_U:
					if (wp->yoff < cur_wp->yoff) ARRAY_ADD(&panes, wp);
					break;
				case WP_D:
					if (wp->yoff > cur_wp->yoff) ARRAY_ADD(&panes, wp);
					break;
			}

	if (ARRAY_LENGTH(&panes) > 0)
		mergesort(ARRAY_DATA(&panes), ARRAY_LENGTH(&panes),
				sizeof(struct window_pane *), sort);

	if (ARRAY_LENGTH(&panes) > 0)
		tar_wp = ARRAY_FIRST(&panes);

	ARRAY_FREE(&panes);
	return tar_wp;
}

// wp_next_l_filt
bool
wp_next_l_filt(struct window_pane *our, struct window_pane *their)
{
	return (our->yoff == their->yoff && their->xoff < our->xoff);
}

// wp_next_l_sort
int
wp_next_l_sort(const void *a, const void *b)
{
	return wp_compare(a, b, true, false);
}

// wp_next_r_file
bool
wp_next_r_filt(struct window_pane *our, struct window_pane *their)
{
	return (our->yoff == their->yoff && their->xoff > our->xoff);
}

// wp_next_r_sort
int
wp_next_r_sort(const void *a, const void *b)
{
	return wp_compare(a, b, true, true);
}

// wp_next_u_filt
bool
wp_next_u_filt(struct window_pane *our, struct window_pane *their)
{
	return (our->xoff == their->xoff && their->yoff < our->yoff);
}

// wp_next_u_sort
int
wp_next_u_sort(const void *a, const void *b)
{
	int r;
	r = wp_compare(a, b, false, false);
	return -(90 - r);
}

// wp_next_d_file
bool
wp_next_d_filt(struct window_pane *our, struct window_pane *their)
{
	return (our->xoff == their->xoff && their->yoff > our->yoff);
}

// wp_next_d_sort
int
wp_next_d_sort(const void *a, const void *b)
{
	return wp_compare(a, b, false, true);
}

// wp_compare
int
wp_compare(const void *a, const void *b, bool xoff, bool sign)
{
	struct window_pane **ra = (struct window_pane **)a;
	struct window_pane **rb = (struct window_pane **)b;

	if (xoff) {
		if (sign)
			return (*ra)->xoff > (*rb)->xoff;
		else
			return (*ra)->xoff < (*rb)->xoff;
	} else {
		if (sign)
			return (*ra)->yoff > (*rb)->yoff;
		else
			return (*ra)->yoff < (*rb)->yoff;
	}
}

