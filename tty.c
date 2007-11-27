/* $Id$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
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
#include <sys/ioctl.h>

#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#define TTYDEFCHARS
/* glibc requires unistd.h before termios.h for TTYDEFCHARS. */
#include <unistd.h>	
#include <termios.h>

#include "tmux.h"

void	tty_fill_acs(struct tty *);
u_char	tty_get_acs(struct tty *, u_char);

void	tty_raw(struct tty *, const char *);
void	tty_puts(struct tty *, const char *);
void	tty_putc(struct tty *, char);

void	tty_attributes(struct tty *, u_char, u_char);
char	tty_translate(char);

void
tty_init(struct tty *tty, char *path, char *term)
{
	tty->path = xstrdup(path);
	tty->term = xstrdup(term);
}

int
tty_open(struct tty *tty, char **cause)
{
	struct termios	 tio;
	int		 error;

	tty->fd = open(tty->path, O_RDWR|O_NONBLOCK);
	if (tty->fd == -1) {
		xasprintf(cause, "%s: %s", tty->path, strerror(errno));
		return (-1);
	}

	if (tty->term == NULL)
		tty->term = xstrdup("unknown");
	if (setupterm(tty->term, tty->fd, &error) != OK) {
		switch (error) {
		case 0:
			xasprintf(cause, "can't use hardcopy terminal");
			break;
		case 1:
			xasprintf(cause, "missing or unsuitable terminal");
			break;
		case 2:
			xasprintf(cause, "can't find terminfo database");
			break;
		default:
			xasprintf(cause, "unknown error");
			break;
		}
		return (-1);
	}
	tty->termp = cur_term;

	tty->in = buffer_create(BUFSIZ);
	tty->out = buffer_create(BUFSIZ);

	tty->attr = SCREEN_DEFATTR;
	tty->colr = SCREEN_DEFCOLR;

	tty_keys_init(tty);
	
	tty_fill_acs(tty);

	if (tcgetattr(tty->fd, &tty->tio) != 0)
		fatal("tcgetattr failed");
	memset(&tio, 0, sizeof tio);
	tio.c_iflag = TTYDEF_IFLAG & ~(IXON|IXOFF|ICRNL|INLCR);
	tio.c_oflag = TTYDEF_OFLAG & ~(OPOST|ONLCR|OCRNL|ONLRET);
	tio.c_lflag = 
	    TTYDEF_LFLAG & ~(IEXTEN|ICANON|ECHO|ECHOE|ECHOKE|ECHOCTL|ISIG);
	tio.c_cflag = TTYDEF_CFLAG;
	memcpy(&tio.c_cc, ttydefchars, sizeof tio.c_cc);
	cfsetspeed(&tio, TTYDEF_SPEED);
	if (tcsetattr(tty->fd, TCSANOW, &tio) != 0)
		fatal("tcsetattr failed");

	if (enter_ca_mode != NULL)
		tty_puts(tty, enter_ca_mode);
	if (keypad_xmit != NULL)
		tty_puts(tty, keypad_xmit);
	if (ena_acs != NULL)
		tty_puts(tty, ena_acs);
	tty_puts(tty, clear_screen);

	return (0);
}

void
tty_close(struct tty *tty)
{
	struct winsize	ws;

	if (ioctl(tty->fd, TIOCGWINSZ, &ws) == -1)
		fatal("ioctl(TIOCGWINSZ)");

	if (keypad_local != NULL)
		tty_raw(tty, keypad_local);
	if (exit_ca_mode != NULL)
		tty_raw(tty, exit_ca_mode);
	tty_raw(tty, clear_screen);
	if (cursor_normal != NULL)
		tty_raw(tty, cursor_normal);
	if (exit_attribute_mode != NULL)
		tty_raw(tty, exit_attribute_mode);
	if (change_scroll_region != NULL)
		tty_raw(tty, tparm(change_scroll_region, 0, ws.ws_row - 1));

	if (tcsetattr(tty->fd, TCSANOW, &tty->tio) != 0)
		fatal("tcsetattr failed");
	del_curterm(tty->termp);

	tty_keys_free(tty);

	close(tty->fd);
	buffer_destroy(tty->in);
	buffer_destroy(tty->out);
}

void
tty_free(struct tty *tty)
{
	if (tty->fd != -1)
		tty_close(tty);

	if (tty->path != NULL)
		xfree(tty->path);
	if (tty->term != NULL)
		xfree(tty->term);
}

void
tty_fill_acs(struct tty *tty)
{
	char	*ptr;

	memset(tty->acs, 0, sizeof tty->acs);
	if (acs_chars == NULL || (strlen(acs_chars) % 2) != 0)
		return;
	for (ptr = acs_chars; *ptr != '\0'; ptr += 2)
		tty->acs[(u_char) ptr[0]] = ptr[1];
}

u_char
tty_get_acs(struct tty *tty, u_char ch)
{
	if (tty->acs[ch] != '\0')
		return (tty->acs[ch]);
	return (ch);
}

void
tty_raw(struct tty *tty, const char *s)
{
	write(tty->fd, s, strlen(s));
}

void
tty_puts(struct tty *tty, const char *s)
{
	buffer_write(tty->out, s, strlen(s));
}

void
tty_putc(struct tty *tty, char ch)
{
	if (tty->attr & ATTR_DRAWING)
		ch = tty_get_acs(tty, ch);
	buffer_write8(tty->out, ch);
}

void
tty_vwrite(struct tty *tty, int cmd, va_list ap)
{
	char	ch;
	u_int	ua, ub;

	set_curterm(tty->termp);

	switch (cmd) {
	case TTY_CHARACTER:
		ch = va_arg(ap, int);
		switch (ch) {
		case '\n':	/* LF */
			tty_puts(tty, cursor_down);
			break;
		case '\r':	/* CR */
			tty_puts(tty, carriage_return);
			break;
		case '\007':	/* BEL */
			if (bell != NULL)
				tty_puts(tty, bell);
			break;
		case '\010':	/* BS */
			tty_puts(tty, cursor_left);
			break;
		default:
			tty_putc(tty, ch);
			break;
		}
		break;
	case TTY_CURSORUP:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_up_cursor, ua));
		break;
	case TTY_CURSORDOWN:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_down_cursor, ua));
		break;
	case TTY_CURSORRIGHT:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_right_cursor, ua));
		break;
	case TTY_CURSORLEFT:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_left_cursor, ua));
		break;
	case TTY_CURSORMOVE:
		ua = va_arg(ap, u_int);
		ub = va_arg(ap, u_int);
		tty_puts(tty, tparm(cursor_address, ua, ub));
		break;
	case TTY_CLEARENDOFLINE:
		tty_puts(tty, clr_eol);
		break;
	case TTY_CLEARSTARTOFLINE:
		tty_puts(tty, clr_bol);
		break;
	case TTY_CLEARLINE:
		tty_puts(tty, clr_eol);	/* XXX */
		break;
	case TTY_INSERTLINE:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_insert_line, ua));
		break;
	case TTY_DELETELINE:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_delete_line, ua));
		break;
	case TTY_INSERTCHARACTER:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_ich, ua));
		break;
	case TTY_DELETECHARACTER:
		ua = va_arg(ap, u_int);
		tty_puts(tty, tparm(parm_dch, ua));
		break;
	case TTY_CURSORON:
		if (cursor_normal != NULL)
			tty_puts(tty, cursor_normal);
		break;
	case TTY_CURSOROFF:
		if (cursor_invisible != NULL)
			tty_puts(tty, cursor_invisible);
		break;
	case TTY_REVERSEINDEX:
		tty_puts(tty, scroll_reverse);
		break;
	case TTY_SCROLLREGION:
		ua = va_arg(ap, u_int);
		ub = va_arg(ap, u_int);
		tty_puts(tty, tparm(change_scroll_region, ua, ub));
		break;
	case TTY_INSERTON:
		if (enter_insert_mode != NULL)
			tty_puts(tty, enter_insert_mode);
		break;
	case TTY_INSERTOFF:
		if (exit_insert_mode != NULL)
			tty_puts(tty, exit_insert_mode);
		break;
#if 0
	case TTY_KCURSOROFF:
		t = tigetstr("CE");
		if (t != (char *) 0 && t != (char *) -1)
			tty_puts(tty, t);
		break;
	case TTY_KCURSORON:
		t = tigetstr("CS");
		if (t != (char *) 0 && t != (char *) -1)
			tty_puts(tty, t);
		break;
	case TTY_KKEYPADOFF:
		if (keypad_local != NULL)
			tty_puts(tty, keypad_local);
		break;
	case TTY_KKEYPADON:
		if (keypad_xmit != NULL)
			tty_puts(tty, keypad_xmit);
#endif
		break;
	case TTY_TITLE:
		break;
	case TTY_ATTRIBUTES:
		ua = va_arg(ap, u_int);
		ub = va_arg(ap, u_int);
		tty_attributes(tty, ua, ub);
		break;
	}
}

void
tty_attributes(struct tty *tty, u_char attr, u_char colr)
{
	u_char	fg, bg;

	if (attr == tty->attr && colr == tty->colr)
		return;

	/* If any bits are being cleared, reset everything. */
	if (tty->attr & ~attr) {
		if ((tty->attr & ATTR_DRAWING) &&
		    exit_alt_charset_mode != NULL)
			tty_puts(tty, exit_alt_charset_mode);
		tty_puts(tty, exit_attribute_mode);
		tty->colr = 0x88;
		tty->attr = 0;
	}

	/* Filter out bits already set. */
	attr &= ~tty->attr;
	tty->attr |= attr;

	if ((attr & ATTR_BRIGHT) && enter_bold_mode != NULL)
		tty_puts(tty, enter_bold_mode);
	if ((attr & ATTR_DIM) && enter_dim_mode != NULL)
		tty_puts(tty, enter_dim_mode);
	if ((attr & ATTR_ITALICS) && enter_standout_mode != NULL)
		tty_puts(tty, enter_standout_mode);
	if ((attr & ATTR_UNDERSCORE) && enter_underline_mode != NULL)
		tty_puts(tty, enter_underline_mode);
	if ((attr & ATTR_BLINK) && enter_blink_mode != NULL)
		tty_puts(tty, enter_blink_mode);
	if ((attr & ATTR_REVERSE) && enter_reverse_mode != NULL)
		tty_puts(tty, enter_reverse_mode);
	if ((attr & ATTR_HIDDEN) && enter_secure_mode != NULL)
		tty_puts(tty, enter_secure_mode);
	if ((attr & ATTR_DRAWING) && enter_alt_charset_mode != NULL)
		tty_puts(tty, enter_alt_charset_mode);

	fg = (colr >> 4) & 0xf;
	if (fg != ((tty->colr >> 4) & 0xf)) {
		if (tigetflag("AX") == TRUE) {
			if (fg == 7)
				fg = 8;
		} else {
			if (fg == 8)
				fg = 7;
		}
		
		if (fg == 8)
			tty_puts(tty, "\e[39m");
		else if (set_a_foreground != NULL)
			tty_puts(tty, tparm(set_a_foreground, fg));
	}
	
	bg = colr & 0xf;
	if (bg != (tty->colr & 0xf)) {
		if (tigetflag("AX") == TRUE) {
			if (bg == 0)
				bg = 8;
		} else {
			if (bg == 8)
				bg = 0;
		}

		if (bg == 8)
			tty_puts(tty, "\e[49m");
		else if (set_a_background != NULL)
			tty_puts(tty, tparm(set_a_background, bg));
	}

	tty->colr = colr;
}