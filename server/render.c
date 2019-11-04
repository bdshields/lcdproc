/** \file server/render.c
 * This file contains code that actually generates the full screen data to
 * send to the LCD. render_screen() takes a screen definition and calls
 * render_frame() which in turn builds the screen according to the definition.
 * It may recursively call itself (for nested frames).
 *
 * This needs to be greatly expanded and redone for greater flexibility.
 * For example, it should support multiple screen sizes, more flexible
 * widgets, and multiple simultaneous screens.
 *
 * This will probably take a while to do.  :(
 *
 * THIS FILE IS MESSY!  Anyone care to rewrite it nicely?  Please??  :)
 *
 * NOTE: (from David Douthitt) Multiple screen sizes?  Multiple simultaneous
 * screens?  Horrors of horrors... next thing you know it'll be making coffee...
 * Better believe it'll take a while to do...
 *
 * \todo Review render_string for correctness.
 */

/* This file is part of LCDd, the lcdproc server.
 *
 * This file is released under the GNU General Public License.
 * Refer to the COPYING file distributed with this package.
 *
 * Copyright (c) 1999, William Ferrell, Selene Scriven
 *		 2001, Joris Robijn
 *		 2007, Peter Marschall
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "shared/report.h"
#include "shared/LL.h"
#include "shared/defines.h"

#include "drivers.h"
#include "screen.h"
#include "screenlist.h"
#include "widget.h"
#include "render.h"

#define BUFSIZE 1024	/* larger than display width => large enough */

int heartbeat = HEARTBEAT_OPEN;
static int heartbeat_fallback = HEARTBEAT_ON; /* If no heartbeat setting has been set at all */

int backlight = BACKLIGHT_OPEN;
static int backlight_fallback = BACKLIGHT_ON; /* If no backlight setting has been set at all */

int titlespeed = 1;

int output_state = 0;
char *server_msg_text;
int server_msg_expire = 0;

#if 1
static void render_frame(LinkedList *list, Loc origin, Box vis, char fscroll, int fspeed, long timer);
#else
static void render_frame(LinkedList *list, int left, int top, int right, int bottom, int fwid, int fhgt, char fscroll, int fspeed, long timer);
#endif
static void render_string(Widget *w, Loc origin, Box vis);
static void render_hbar(Widget *w, int left, int top, int right, int bottom, int fy);
static void render_vbar(Widget *w, int left, int top, int right, int bottom);
static void render_pbar(Widget *w, int left, int top, int right, int bottom);
static void render_title(Widget *w, Loc origin, Box vis, long timer);
static void render_scroller(Widget *w, int left, int top, int right, int bottom, long timer);
static void render_num(Widget *w, int left, int top, int right, int bottom);
static int calc_scrolling(int speed, int timer, int bounce, int space);
static Box calc_intersection(Box first, Box second);

/**
 * Renders a screen. The following actions are taken in order:
 *
 * \li  Clear the screen.
 * \li  Set the backlight.
 * \li  Set out-of-band data (output).
 * \li  Render the frame contents.
 * \li  Set the cursor.
 * \li  Draw the heartbeat.
 * \li  Show any server message.
 * \li  Flush all output to screen.
 *
 * \param s      The screen to render.
 * \param timer  A value increased with every call.
 * \return  -1 on error, 0 on success.
 */
int
render_screen(Screen *s, long timer)
{
	int tmp_state = 0;

	debug(RPT_DEBUG, "%s(screen=[%.40s], timer=%ld)  ==== START RENDERING ====", __FUNCTION__, s->id, timer);

	if (s == NULL)
		return -1;

	/* 1. Clear the LCD screen... */
	drivers_clear();

	/* 2. Set up the backlight */
	/*-
	 * 2.1:
	 * First we find out who has set the backlight:
	 *   a) the screen,
	 *   b) the client, or
	 *   c) the server core
	 * with the latter taking precedence over the earlier. If the
	 * backlight is not set on/off then use the fallback (set it ON).
	 */
	if (backlight != BACKLIGHT_OPEN) {
		tmp_state = backlight;
	}
	else if ((s->client != NULL) && (s->client->backlight != BACKLIGHT_OPEN)) {
		tmp_state = s->client->backlight;
	}
	else if (s->backlight != BACKLIGHT_OPEN) {
		tmp_state = s->backlight;
	}
	else {
		tmp_state = backlight_fallback;
	}

	/*-
	 * 2.2:
	 * If one of the backlight options (FLASH or BLINK) has been set turn
	 * it on/off based on a timed algorithm.
	 */
	/* NOTE: dirty stripping of other options... */
	/* Backlight flash: check timer and flip backlight as appropriate */
	if (tmp_state & BACKLIGHT_FLASH) {
		drivers_backlight(
			(
				(tmp_state & BACKLIGHT_ON)
				^ ((timer & 7) == 7)
			) ? BACKLIGHT_ON : BACKLIGHT_OFF);
	}
	/* Backlight blink: check timer and flip backlight as appropriate */
	else if (tmp_state & BACKLIGHT_BLINK) {
		drivers_backlight(
			(
				(tmp_state & BACKLIGHT_ON)
				^ ((timer & 14) == 14)
			) ? BACKLIGHT_ON : BACKLIGHT_OFF);
	}
	else {
		/* Simple: Only send lowest bit then... */
		drivers_backlight(tmp_state & BACKLIGHT_ON);
	}

	/* 3. Output ports from LCD - outputs depend on the current screen */
	drivers_output(output_state);

	/* 4. Draw a frame... */
	render_frame(s->widgetlist, _LOC(0, 0 ,s->width, s->height),
			_BOX(0,0,display_props->width, display_props->height),
			'v', max(s->duration / s->height, 1), timer);

	/* 5. Set the cursor */
	drivers_cursor(s->cursor_x, s->cursor_y, s->cursor);

	/* 6. Set the heartbeat */
	if (heartbeat != HEARTBEAT_OPEN) {
		tmp_state = heartbeat;
	}
	else if ((s->client != NULL) && (s->client->heartbeat != HEARTBEAT_OPEN)) {
		tmp_state = s->client->heartbeat;
	}
	else if (s->heartbeat != HEARTBEAT_OPEN) {
		tmp_state = s->heartbeat;
	}
	else {
		tmp_state = heartbeat_fallback;
	}
	drivers_heartbeat(tmp_state);

	/* 7. If there is an server message that is not expired, display it */
	if (server_msg_expire > 0) {
		drivers_string(display_props->width - strlen(server_msg_text) + 1,
				display_props->height, server_msg_text);
		server_msg_expire--;
		if (server_msg_expire == 0) {
			free(server_msg_text);
		}
	}

	/* 8. Flush display out, frame and all... */
	drivers_flush();

	debug(RPT_DEBUG, "==== END RENDERING ====");
	return 0;

}

/* The following function is positively ghastly (as was mentioned above!) */
/* Best thing to do is to remove support for frames... but anyway... */
/* */
static void
render_frame(LinkedList *list,
		Loc origin,
		Box vis,
#if 0
		int left,	/* left edge of frame */
		int top,	/* top edge of frame */
		int right,	/* right edge of frame */
		int bottom,	/* bottom edge of frame */
		int fwid,	/* frame width? */
		int fhgt,	/* frame height? */
#endif
		char fscroll,	/* direction of scrolling */
		int fspeed,	/* speed of scrolling... */
		long timer)	/* current timer tick */
{
    int fx;
    int fy;

	debug(RPT_DEBUG, "%s(list=%p, x=%d, y=%d, width=%d, height=%d "
			  "left=%d, top=%d, right=%d, bottom=%d, "
			  "fscroll='%c', fspeed=%d, timer=%ld)",
			  __FUNCTION__, list, origin.x, origin.y, origin.width, origin.height,
			  vis.left, vis.top, vis.right, vis.bottom,
			  fscroll, fspeed, timer);

	/* return on no data or illegal height */
	if ((list == NULL) || (origin.height <= 0) || (origin.width <= 0))
		return;

	if (fscroll == 'v') {		/* vertical scrolling */
		// only set offset when there is something to scroll
		if (origin.height > (vis.bottom - vis.top)) {
			int fy_max = origin.height - (vis.bottom - vis.top) + 1;
			fy = calc_scrolling(fspeed, timer, 1, fy_max);
			debug(RPT_DEBUG, "%s: fy=%d", __FUNCTION__, fy);
			origin.y -= fy;
		}
	}
	else if (fscroll == 'h') {	/* horizontal scrolling */
		// only set offset when there is something to scroll
		if (origin.width > (vis.right - vis.left)) {
			int fx_max = origin.width - (vis.right - vis.left) + 1;
			fx = calc_scrolling(fspeed, timer, 1, fx_max);
			debug(RPT_DEBUG, "%s: fx=%d", __FUNCTION__, fx);
			origin.x -= fx;
		}
	}


	/* reset widget list */
	LL_Rewind(list);

	/* loop over all widgets */
	do {
		Widget *w = (Widget *) LL_Get(list);

		if (w == NULL)
			return;

		/* TODO:  Make this cleaner and more flexible! */
		switch (w->type) {
		case WID_STRING:
			render_string(w, origin,vis);
			break;
#if 0
		case WID_HBAR:
			render_hbar(w, left, top, right, bottom, 0);
			break;
		case WID_VBAR:	  /* FIXME:  Vbars don't work in frames! */
			render_vbar(w, left, top, right, bottom);
			break;
		case WID_PBAR:
			render_pbar(w, left, top, right, bottom);
			break;
		case WID_ICON:	  /* FIXME:  Icons don't work in frames! */
			drivers_icon(w->x, w->y, w->length);
			break;
#endif
		case WID_TITLE:	  /* FIXME:  Doesn't work quite right in frames... */
			render_title(w, origin,vis, timer);
			break;
#if 0
		case WID_SCROLLER: /* FIXME: doesn't work in frames... */
			render_scroller(w, left, top, right, bottom, timer);
			break;
#endif
		case WID_FRAME:
			{
				Box visible;
				visible = calc_intersection(vis, _BOX(origin.x + w->left-1, origin.y + w->top-1, origin.x + w->right, origin.y + w->bottom));

				if ((visible.left == 0) && (visible.right == 0)) {
					debug(RPT_DEBUG, "%s(list=%p, x=%d, y=%d, width=%d, height=%d "
							  "left=%d, top=%d, right=%d, bottom=%d, "
							  "fscroll='%c', fspeed=%d, timer=%ld)",
							  "drop_frame", list, origin.x, origin.y, w->width, w->height,
							  w->left, w->top, w->right, w->bottom,
							  w->length, w->speed, timer);
				}
				else {
					render_frame(w->frame_screen->widgetlist, _LOC(origin.x+w->left - 1, origin.y + w->top -1, w->width, w->height), visible,
							w->length, w->speed, timer);
				}
			}
			break;
#if 0
		case WID_NUM:	  /* FIXME: doesn't work in frames... */
			/* NOTE: y=10 means COLON (:) */
			if ((w->x > 0) && (w->y >= 0) && (w->y <= 10)) {
				drivers_num(w->x + left, w->y);
			}
			break;
#endif
		case WID_NONE:
			/* FALLTHROUGH */
		default:
			break;
		}
	} while (LL_Next(list) == 0);
}

/**
 * Calculate the intersection of two boxes
 */
static Box
calc_intersection(Box first, Box second)
{
	Box intersect;


	// Check if no overlap at all
	if((first.right < second.left) || (first.bottom < second.top)
			|| (first.left > second.right) || (first.top > second.bottom)) {
		return (Box){0,0,0,0};
	}

	// Left Edge
	if(first.left >= second.left) {
		intersect.left = first.left;
	}
	else {
		intersect.left = second.left;
	}

	// Right Edge
	if(first.right < second.right) {
		intersect.right = first.right;
	}
	else {
		intersect.right = second.right;
	}

	// Top Edge
	if(first.top < second.top) {
		intersect.top = second.top;
	}
	else {
		intersect.top = first.top;
	}

	// bottom Edge
	if(first.bottom > second.bottom) {
		intersect.bottom = second.bottom;
	}
	else {
		intersect.bottom = first.bottom;
	}
#if 0  // Print calculated intersection
	debug(RPT_DEBUG, "%s((left=%d, top=%d, right=%d, bottom=%d),(left=%d, top=%d, right=%d, bottom=%d),"
								"(left=%d, top=%d, right=%d, bottom=%d))",
								  __FUNCTION__,  first.left, first.top, first.right, first.bottom,
								  second.left, second.top, second.right, second.bottom,
								  intersect.left, intersect.top, intersect.right, intersect.bottom);
#endif
	return intersect;
}

/**
 * Calculate the scrolling position
 *
 */
static int
calc_scrolling(int speed, int timer, int bounce, int space)
{
	int offset;     // Where in the scrolling cycle we are up to
	int increments;
	int directions;
	int extra = 0;   // Extra delay for both ends of scrolling
	if(bounce) {
		directions = 2;
		extra = 20;
	}
	else {
		directions = 1;
	}
	if (speed > 0) {
		increments = (space + extra) * speed;
		if (((timer / increments) % directions) == 0) {
			/* wiggle one way */
			offset = (timer % increments)  / speed;
		}
		else {
			/* wiggle the other */
			offset = (((timer % increments) - increments + 1) / speed) * -1;
		}
	}
	else if (speed < 0) {
		increments = (space + extra) / (speed * -1);
		if (((timer / increments) % directions) == 0) {
			offset = (timer % increments) * speed * -1;
		}
		else {
			offset = (((timer % increments) * speed * -1) - (space + extra) + 1) * -1;
		}
	}
	else {
		offset = 0;
	}
	offset -= extra/2;
	if(offset < 0)
	{
		offset = 0;
	}
	else if(offset > space)
	{
		offset = space;
	}
	return offset;
}


static void
render_string(Widget *w, Loc origin, Box vis)
{
	debug(RPT_DEBUG, "%s(w=%p, x=%d, y=%d, width=%d, height=%d "
			  "left=%d, top=%d, right=%d, bottom=%d, string=%s)",
			  __FUNCTION__, w, origin.x, origin.y, origin.width, origin.height,
			  vis.left, vis.top, vis.right, vis.bottom, w->text);
	int length, offset;
	if (w->text != NULL) {
		// Check if the widget is on a visible line
		if((origin.y + w->y > vis.top) && (origin.y + w->y <= vis.bottom)) {
			length = strlen(w->text);
			// Calculate the offset of the first visible character
			offset = vis.left - (origin.x + w->x) + 1;

			if(offset < 0) {
				// If the offset is less than zero, then the widget is indented from the edge of the frame
				offset = 0;
			}
			else if(offset > length) {
				// If the offset is greater than the length of the string, then none of the string is visible
				offset = length;
			}

			// TODO: Duplicate and truncate the string so that only what will fit is written.

			drivers_string(origin.x + w->x + offset, origin.y + w->y, w->text + offset);
		}
	}
}


static void
render_hbar(Widget *w, int left, int top, int right, int bottom, int fy)
{
	debug(RPT_DEBUG, "%s(w=%p, left=%d, top=%d, right=%d, bottom=%d, fy=%d)",
			  __FUNCTION__, w, left, top, right, bottom, fy);

	if (!((w->x > 0) && (w->y > 0) && (w->y > fy) && (w->y <= bottom - top)))
		return;

	if (w->length > 0) {
		int len = display_props->width - w->x - left + 1;
		int promille = 1000;

		if ((w->length / display_props->cellwidth) < right - left - w->x + 1) {
			len = w->length / display_props->cellwidth +
			      (w->length % display_props->cellwidth ? 1 : 0);
			promille = (long) 1000 * w->length /
				   (display_props->cellwidth * len);
		}

		drivers_hbar(w->x + left, w->y + top, len, promille, BAR_PATTERN_FILLED);
	}
	else if (w->length < 0) {
		/* TODO:  Rearrange stuff to get left-extending
		 * hbars to draw correctly...
		 * .. er, this'll require driver modifications,
		 * so I'll leave it out for now.
		 */
	}
}


static void
render_vbar(Widget *w, int left, int top, int right, int bottom)
{
	debug(RPT_DEBUG, "%s(w=%p, left=%d, top=%d, right=%d, bottom=%d)",
			  __FUNCTION__, w, left, top, right, bottom);

	if (!((w->x > 0) && (w->y > 0)))
		return;

	if (w->length > 0) {
		int full_len = display_props->height;
		int promille = (long) 1000 * w->length / (display_props->cellheight * full_len);

		drivers_vbar(w->x + left, w->y + top, full_len, promille, BAR_PATTERN_FILLED);
	}
	else if (w->length < 0) {
		/* TODO:  Rearrange stuff to get down-extending
		 * vbars to draw correctly...
		 * .. er, this'll require driver modifications,
		 * so I'll leave it out for now.
		 */
	}
}

static void
render_pbar(Widget *w, int left, int top, int right, int bottom)
{
	debug(RPT_DEBUG, "%s(w=%p, left=%d, top=%d, right=%d, bottom=%d)",
			  __FUNCTION__, w, left, top, right, bottom);

	if (!((w->x > 0) && (w->y > 0) && (w->width > 0)))
		return;

	drivers_pbar(w->x + left, w->y + top, w->width, w->promille,
			w->begin_label, w->end_label);
}

static void
render_title(Widget *w, Loc origin, Box vis, long timer)
{
	char str[BUFSIZE];
	int x, width = origin.width - 6, length, delay;
	int offset;

	debug(RPT_DEBUG, "%s(w=%p, x=%d, y=%d, width=%d, height=%d "
			  "left=%d, top=%d, right=%d, bottom=%d, timer=%ld)",
			  __FUNCTION__, w, origin.x, origin.y, origin.width, origin.height,
			  vis.left, vis.top, vis.right, vis.bottom, timer);

	if ((w->text == NULL) || (origin.width < 8) || ((origin.y + w->y) < vis.top))
		return;

	length = strlen(w->text);

	/* calculate delay from titlespeed: <=0 -> 0, [1 - infty] -> [10 - 1] */
	delay = (titlespeed <= TITLESPEED_NO)
		? TITLESPEED_NO
		: max(TITLESPEED_MIN, TITLESPEED_MAX - titlespeed);

	/* display leading fillers */
	if(origin.x + w->x > vis.left){
		drivers_icon(origin.x + w->x, origin.y + w->y, ICON_BLOCK_FILLED);
	}
	if(origin.x + w->x + 1 > vis.left){
		drivers_icon(origin.x + w->x + 1, origin.y + w->y, ICON_BLOCK_FILLED);
	}

	length = min(length, sizeof(str)-1);
	if ((length <= width) || (delay == 0)) {
		int max;
		offset = vis.left - (origin.x + w->x + 3) + 1 ;
		if(offset < 0) {
			// If the offset is less than zero, then the widget is indented from the edge of the frame
			offset = 0;
		}
		else if(offset > length) {
			// If the offset is greater than the length of the string, then none of the string is visible
			offset = length;
		}

		length = min(length, width);
		strncpy(str, w->text+offset, length);
		str[length] = '\0';

		/* set x value for trailing fillers */
		x = origin.x + w->x + length + 4;
	}
	else {			/* Scroll the title, if it doesn't fit... */
		int offset = timer;
		int reverse;

		offset = calc_scrolling(delay, timer, 1, length - width);

		/* copy test starting from offset */
		length = min(width, sizeof(str)-1);
		strncpy(str, w->text + offset, length);
		str[length] = '\0';

		/* set x value for trailing fillers */
		x = origin.x + w->x + length + 4;
	}

	/* display text */
	drivers_string(origin.x + w->x + 3 + offset, origin.y + w->y, str);

	/* display trailing fillers */
	for ( ; x <= vis.right; x++) {
		drivers_icon(x, origin.y + w->y, ICON_BLOCK_FILLED);
	}
}


static void
render_scroller(Widget *w, int left, int top, int right, int bottom, long timer)
{
	char str[BUFSIZE];
	int length;
	int offset, gap;
	int screen_width;
	int necessaryTimeUnits = 0;

	debug(RPT_DEBUG, "%s(w=%p, left=%d, top=%d, right=%d, bottom=%d, timer=%ld)",
			  __FUNCTION__, w, left, top, right, bottom, timer);

	if ((w->text == NULL) || (w->right < w->left))
		return;

	screen_width = abs(w->right - w->left + 1);
	screen_width = min(screen_width, sizeof(str)-1);

	switch (w->length) {	/* actually, direction... */
	case 'm': // Marquee
		length = strlen(w->text);
		if (length <= screen_width) {
			/* it fits within the box, just render it */
			drivers_string(w->left, w->top, w->text);
			break;
		}

		gap = screen_width / 2;
		length += gap; /* Allow gap between end and beginning */

		offset = calc_scrolling(w->speed, timer, 0, length);
#if 0
		if (w->speed > 0) {
			necessaryTimeUnits = length * w->speed;
			offset = (timer % necessaryTimeUnits) / w->speed;
		}
		else if (w->speed < 0) {
			necessaryTimeUnits = length / (w->speed * -1);
			offset = (timer % necessaryTimeUnits) * w->speed * -1;
		}
		else {
			offset = 0;
		}
#endif
		if (offset <= length) {
			if (gap > offset) {
				memset(str, ' ', gap - offset);
				strncpy(&str[gap-offset], w->text, screen_width);
			}
			else {
				int room = screen_width - (length - offset);

				strncpy(str, &w->text[offset-gap], screen_width);
				if (room > 0) {
					memset(&str[length-offset], ' ', min(room, gap));
					room -= gap;
					if (room > 0)
						strncpy(&str[length-offset+gap], w->text, room);
				}
			}
			str[screen_width] = '\0';
			drivers_string(w->left, w->top, str);
		}
		break;
	case 'h':
		length = strlen(w->text) + 1;
		if (length <= screen_width) {
			/* it fits within the box, just render it */
			drivers_string(w->left, w->top, w->text);
		}
		else {
			int effLength = length - screen_width;
			offset = calc_scrolling(w->speed, timer, 1, effLength);
#if 0
			if (w->speed > 0) {
				necessaryTimeUnits = effLength * w->speed;
				if (((timer / necessaryTimeUnits) % 2) == 0) {
					/* wiggle one way */
					offset = (timer % (effLength*w->speed))
						 / w->speed;
				}
				else {
					/* wiggle the other */
					offset = (((timer % (effLength * w->speed))
						  - (effLength * w->speed) + 1)
						 / w->speed) * -1;
				}
			}
			else if (w->speed < 0) {
				necessaryTimeUnits = effLength / (w->speed * -1);
				if (((timer / necessaryTimeUnits) % 2) == 0) {
					offset = (timer % (effLength / (w->speed * -1)))
						 * w->speed * -1;
				}
				else {
					offset = (((timer % (effLength / (w->speed * -1)))
						  * w->speed * -1)
						  - effLength + 1) * -1;
				}
			}
			else {
				offset = 0;
			}
#endif
			if (offset <= length) {
				strncpy(str, &((w->text)[offset]), screen_width);
				str[screen_width] = '\0';
				drivers_string(w->left, w->top, str);
				/*debug(RPT_DEBUG, "scroller %s : %d", str, length-offset); */
			}
		}
		break;

	/* FIXME:  Vert scrollers don't always seem to scroll */
	/* back up after hitting the bottom.  They jump back to */
	/* the top instead...  (nevermind?) */
	case 'v':
		length = strlen(w->text);
		if (length <= screen_width) {
			/* no scrolling required... */
			drivers_string(w->left, w->top, w->text);
		}
		else {
			int lines_required = (length / screen_width)
				 + (length % screen_width ? 1 : 0);
			int available_lines = (w->bottom - w->top + 1);

			if (lines_required <= available_lines) {
				/* easy... */
				int i;

				for (i = 0; i < lines_required; i++) {
					strncpy(str, &((w->text)[i * screen_width]), screen_width);
					str[screen_width] = '\0';
					drivers_string(w->left, w->top + i, str);
				}
			}
			else {
				int effLines = lines_required - available_lines + 1;
				int begin = 0;
				int i = 0;

				/*debug(RPT_DEBUG, "length: %d sw: %d lines req: %d  avail lines: %d  effLines: %d ",length,screen_width,lines_required,available_lines,effLines);*/
				begin = calc_scrolling(w->speed, timer, 1, effLines);
#if 0
				if (w->speed > 0) {
					necessaryTimeUnits = effLines * w->speed;
					if (((timer / necessaryTimeUnits) % 2) == 0) {
						/*debug(RPT_DEBUG, "up ");*/
						begin = (timer % (necessaryTimeUnits))
							 / w->speed;
					}
					else {
						/*debug(RPT_DEBUG, "down ");*/
						begin = (((timer % necessaryTimeUnits)
							 - necessaryTimeUnits + 1) / w->speed)
							 * -1;
					}
				}
				else if (w->speed < 0) {
					necessaryTimeUnits = effLines / (w->speed * -1);
					if (((timer / necessaryTimeUnits) % 2) == 0) {
						begin = (timer % necessaryTimeUnits)
							 * w->speed * -1;
					}
					else {
						begin = (((timer % necessaryTimeUnits)
							 * w->speed * -1) - effLines + 1)
							 * -1;
					}
				}
				else {
					begin = 0;
				}
#endif
				/*debug(RPT_DEBUG, "rendering begin: %d  timer: %d effLines: %d",begin,timer,effLines); */
				for (i = begin; i < begin + available_lines; i++) {
					strncpy(str, &((w->text)[i * (screen_width)]), screen_width);
					str[screen_width] = '\0';
					/*debug(RPT_DEBUG, "rendering: '%s' of %s", */
					/*str,w->text); */
					drivers_string(w->left, w->top + (i - begin), str);
				}
			}
		}
		break;
	}
}


static void render_num(Widget *w, int left, int top, int right, int bottom)
{
	debug(RPT_DEBUG, "%s(w=%p, left=%d, top=%d, right=%d, bottom=%d)",
			  __FUNCTION__, w, left, top, right, bottom);

	/* NOTE: y=10 means COLON (:) */
	if ((w->x > 0) && (w->y >= 0) && (w->y <= 10)) {
		drivers_num(w->x + left, w->y);
	}
}


int
server_msg(const char *text, int expire)
{
	debug(RPT_DEBUG, "%s(text=\"%.40s\", expire=%d)", __FUNCTION__, text, expire);

	if (strlen(text) > 15 || expire <= 0) {
		return -1;
	}

	/* Still a message active ? */

	if (server_msg_expire > 0) {
		free(server_msg_text);
	}

	/* Store new message */
	server_msg_text = malloc(strlen(text) + 3);
	strcpy(server_msg_text, "| ");
	strcat(server_msg_text, text);

	server_msg_expire = expire;

	return 0;
}
