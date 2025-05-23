/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Windows
 *
 * Licensed under the Apache License, Version 2.0 (the "License");n
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/cursorfont.h>

#include <winpr/assert.h>
#include <winpr/cast.h>

#include "xf_floatbar.h"
#include "xf_utils.h"
#include "resource/close.xbm"
#include "resource/lock.xbm"
#include "resource/unlock.xbm"
#include "resource/minimize.xbm"
#include "resource/restore.xbm"

#include <freerdp/log.h>
#define TAG CLIENT_TAG("x11")

#define FLOATBAR_HEIGHT 26
#define FLOATBAR_DEFAULT_WIDTH 576
#define FLOATBAR_MIN_WIDTH 200
#define FLOATBAR_BORDER 24
#define FLOATBAR_BUTTON_WIDTH 24
#define FLOATBAR_COLOR_BACKGROUND "RGB:31/6c/a9"
#define FLOATBAR_COLOR_BORDER "RGB:75/9a/c8"
#define FLOATBAR_COLOR_FOREGROUND "RGB:FF/FF/FF"

#define XF_FLOATBAR_MODE_NONE 0
#define XF_FLOATBAR_MODE_DRAGGING 1
#define XF_FLOATBAR_MODE_RESIZE_LEFT 2
#define XF_FLOATBAR_MODE_RESIZE_RIGHT 3

#define XF_FLOATBAR_BUTTON_CLOSE 1
#define XF_FLOATBAR_BUTTON_RESTORE 2
#define XF_FLOATBAR_BUTTON_MINIMIZE 3
#define XF_FLOATBAR_BUTTON_LOCKED 4

typedef BOOL (*OnClick)(xfFloatbar*);

typedef struct
{
	int x;
	int y;
	int type;
	bool focus;
	bool clicked;
	OnClick onclick;
	Window handle;
} xfFloatbarButton;

struct xf_floatbar
{
	int x;
	int y;
	int width;
	int height;
	int mode;
	int last_motion_x_root;
	int last_motion_y_root;
	BOOL locked;
	xfFloatbarButton* buttons[4];
	Window handle;
	BOOL hasCursor;
	xfContext* xfc;
	DWORD flags;
	BOOL created;
	Window root_window;
	char* title;
	XFontSet fontSet;
};

static xfFloatbarButton* xf_floatbar_new_button(xfFloatbar* floatbar, int type);

static BOOL xf_floatbar_button_onclick_close(xfFloatbar* floatbar)
{
	if (!floatbar)
		return FALSE;

	return freerdp_abort_connect_context(&floatbar->xfc->common.context);
}

static BOOL xf_floatbar_button_onclick_minimize(xfFloatbar* floatbar)
{
	xfContext* xfc = NULL;

	if (!floatbar || !floatbar->xfc)
		return FALSE;

	xfc = floatbar->xfc;
	xf_SetWindowMinimized(xfc, xfc->window);
	return TRUE;
}

static BOOL xf_floatbar_button_onclick_restore(xfFloatbar* floatbar)
{
	if (!floatbar)
		return FALSE;

	xf_toggle_fullscreen(floatbar->xfc);
	return TRUE;
}

static BOOL xf_floatbar_button_onclick_locked(xfFloatbar* floatbar)
{
	if (!floatbar)
		return FALSE;

	floatbar->locked = (floatbar->locked) ? FALSE : TRUE;
	return xf_floatbar_hide_and_show(floatbar);
}

BOOL xf_floatbar_set_root_y(xfFloatbar* floatbar, int y)
{
	if (!floatbar)
		return FALSE;

	floatbar->last_motion_y_root = y;
	return TRUE;
}

BOOL xf_floatbar_hide_and_show(xfFloatbar* floatbar)
{
	xfContext* xfc = NULL;

	if (!floatbar || !floatbar->xfc)
		return FALSE;

	if (!floatbar->created)
		return TRUE;

	xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);

	if (!floatbar->locked)
	{
		if ((floatbar->mode == XF_FLOATBAR_MODE_NONE) && (floatbar->last_motion_y_root > 10) &&
		    (floatbar->y > (FLOATBAR_HEIGHT * -1)))
		{
			floatbar->y = floatbar->y - 1;
			LogDynAndXMoveWindow(xfc->log, xfc->display, floatbar->handle, floatbar->x,
			                     floatbar->y);
		}
		else if (floatbar->y < 0 && (floatbar->last_motion_y_root < 10))
		{
			floatbar->y = floatbar->y + 1;
			LogDynAndXMoveWindow(xfc->log, xfc->display, floatbar->handle, floatbar->x,
			                     floatbar->y);
		}
	}

	return TRUE;
}

static BOOL create_floatbar(xfFloatbar* floatbar)
{
	xfContext* xfc = NULL;
	Status status = 0;
	XWindowAttributes attr = { 0 };

	WINPR_ASSERT(floatbar);
	if (floatbar->created)
		return TRUE;

	xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);

	status = XGetWindowAttributes(xfc->display, floatbar->root_window, &attr);
	if (status == 0)
	{
		WLog_WARN(TAG, "XGetWindowAttributes failed");
		return FALSE;
	}
	floatbar->x = attr.x + attr.width / 2 - FLOATBAR_DEFAULT_WIDTH / 2;
	floatbar->y = 0;

	if (((floatbar->flags & 0x0004) == 0) && !floatbar->locked)
		floatbar->y = -FLOATBAR_HEIGHT + 1;

	floatbar->handle = LogDynAndXCreateWindow(
	    xfc->log, xfc->display, floatbar->root_window, floatbar->x, 0, FLOATBAR_DEFAULT_WIDTH,
	    FLOATBAR_HEIGHT, 0, CopyFromParent, InputOutput, CopyFromParent, 0, NULL);
	floatbar->width = FLOATBAR_DEFAULT_WIDTH;
	floatbar->height = FLOATBAR_HEIGHT;
	floatbar->mode = XF_FLOATBAR_MODE_NONE;
	floatbar->buttons[0] = xf_floatbar_new_button(floatbar, XF_FLOATBAR_BUTTON_CLOSE);
	floatbar->buttons[1] = xf_floatbar_new_button(floatbar, XF_FLOATBAR_BUTTON_RESTORE);
	floatbar->buttons[2] = xf_floatbar_new_button(floatbar, XF_FLOATBAR_BUTTON_MINIMIZE);
	floatbar->buttons[3] = xf_floatbar_new_button(floatbar, XF_FLOATBAR_BUTTON_LOCKED);
	XSelectInput(xfc->display, floatbar->handle,
	             ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
	                 FocusChangeMask | LeaveWindowMask | EnterWindowMask | StructureNotifyMask |
	                 PropertyChangeMask);
	floatbar->created = TRUE;
	return TRUE;
}

BOOL xf_floatbar_toggle_fullscreen(xfFloatbar* floatbar, bool fullscreen)
{
	int size = 0;
	bool visible = False;
	xfContext* xfc = NULL;

	if (!floatbar || !floatbar->xfc)
		return FALSE;

	xfc = floatbar->xfc;
	WINPR_ASSERT(xfc->display);

	/* Only visible if enabled */
	if (floatbar->flags & 0x0001)
	{
		/* Visible if fullscreen and flag visible in fullscreen mode */
		visible |= ((floatbar->flags & 0x0010) != 0) && fullscreen;
		/* Visible if window and flag visible in window mode */
		visible |= ((floatbar->flags & 0x0020) != 0) && !fullscreen;
	}

	if (visible)
	{
		if (!create_floatbar(floatbar))
			return FALSE;

		LogDynAndXMapWindow(xfc->log, xfc->display, floatbar->handle);
		size = ARRAYSIZE(floatbar->buttons);

		for (int i = 0; i < size; i++)
		{
			xfFloatbarButton* button = floatbar->buttons[i];
			LogDynAndXMapWindow(xfc->log, xfc->display, button->handle);
		}

		/* If default is hidden (and not sticky) don't show on fullscreen state changes */
		if (((floatbar->flags & 0x0004) == 0) && !floatbar->locked)
			floatbar->y = -FLOATBAR_HEIGHT + 1;

		xf_floatbar_hide_and_show(floatbar);
	}
	else if (floatbar->created)
	{
		XUnmapSubwindows(xfc->display, floatbar->handle);
		LogDynAndXUnmapWindow(xfc->log, xfc->display, floatbar->handle);
	}

	return TRUE;
}

xfFloatbarButton* xf_floatbar_new_button(xfFloatbar* floatbar, int type)
{
	xfFloatbarButton* button = NULL;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(floatbar->xfc);
	WINPR_ASSERT(floatbar->xfc->display);
	WINPR_ASSERT(floatbar->handle);

	button = (xfFloatbarButton*)calloc(1, sizeof(xfFloatbarButton));
	button->type = type;

	switch (type)
	{
		case XF_FLOATBAR_BUTTON_CLOSE:
			button->x = floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * type;
			button->onclick = xf_floatbar_button_onclick_close;
			break;

		case XF_FLOATBAR_BUTTON_RESTORE:
			button->x = floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * type;
			button->onclick = xf_floatbar_button_onclick_restore;
			break;

		case XF_FLOATBAR_BUTTON_MINIMIZE:
			button->x = floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * type;
			button->onclick = xf_floatbar_button_onclick_minimize;
			break;

		case XF_FLOATBAR_BUTTON_LOCKED:
			button->x = FLOATBAR_BORDER;
			button->onclick = xf_floatbar_button_onclick_locked;
			break;

		default:
			break;
	}

	button->y = 0;
	button->focus = FALSE;
	button->handle =
	    LogDynAndXCreateWindow(floatbar->xfc->log, floatbar->xfc->display, floatbar->handle,
	                           button->x, 0, FLOATBAR_BUTTON_WIDTH, FLOATBAR_BUTTON_WIDTH, 0,
	                           CopyFromParent, InputOutput, CopyFromParent, 0, NULL);
	XSelectInput(floatbar->xfc->display, button->handle,
	             ExposureMask | ButtonPressMask | ButtonReleaseMask | FocusChangeMask |
	                 LeaveWindowMask | EnterWindowMask | StructureNotifyMask);
	return button;
}

xfFloatbar* xf_floatbar_new(xfContext* xfc, Window window, const char* name, DWORD flags)
{
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);
	WINPR_ASSERT(name);

	/* Floatbar not enabled */
	if ((flags & 0x0001) == 0)
		return NULL;

	if (!xfc)
		return NULL;

	/* Force disable with remote app */
	if (xfc->remote_app)
		return NULL;

	xfFloatbar* floatbar = (xfFloatbar*)calloc(1, sizeof(xfFloatbar));

	if (!floatbar)
		return NULL;

	floatbar->title = _strdup(name);

	if (!floatbar->title)
		goto fail;

	floatbar->root_window = window;
	floatbar->flags = flags;
	floatbar->xfc = xfc;
	floatbar->locked = (flags & 0x0002) != 0 ? TRUE : FALSE;
	xf_floatbar_toggle_fullscreen(floatbar, FALSE);
	char** missingList = NULL;
	int missingCount = 0;
	char* defString = NULL;
	floatbar->fontSet = XCreateFontSet(floatbar->xfc->display, "-*-*-*-*-*-*-*-*-*-*-*-*-*-*",
	                                   &missingList, &missingCount, &defString);
	if (floatbar->fontSet == NULL)
	{
		WLog_ERR(TAG, "Failed to create fontset");
	}
	XFreeStringList(missingList);
	return floatbar;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	xf_floatbar_free(floatbar);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

static unsigned long xf_floatbar_get_color(xfFloatbar* floatbar, char* rgb_value)
{
	XColor color;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(floatbar->xfc);

	Display* display = floatbar->xfc->display;
	WINPR_ASSERT(display);

	Colormap cmap = DefaultColormap(display, XDefaultScreen(display));
	XParseColor(display, cmap, rgb_value, &color);
	XAllocColor(display, cmap, &color);
	return color.pixel;
}

static void xf_floatbar_event_expose(xfFloatbar* floatbar)
{
	GC gc = NULL;
	GC shape_gc = NULL;
	Pixmap pmap = 0;
	XPoint shape[5] = { 0 };
	XPoint border[5] = { 0 };

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(floatbar->xfc);

	Display* display = floatbar->xfc->display;
	WINPR_ASSERT(display);

	/* create the pixmap that we'll use for shaping the window */
	pmap = LogDynAndXCreatePixmap(floatbar->xfc->log, display, floatbar->handle,
	                              WINPR_ASSERTING_INT_CAST(uint32_t, floatbar->width),
	                              WINPR_ASSERTING_INT_CAST(uint32_t, floatbar->height), 1);
	gc = LogDynAndXCreateGC(floatbar->xfc->log, display, floatbar->handle, 0, 0);
	shape_gc = LogDynAndXCreateGC(floatbar->xfc->log, display, pmap, 0, 0);
	/* points for drawing the floatbar */
	shape[0].x = 0;
	shape[0].y = 0;
	shape[1].x = WINPR_ASSERTING_INT_CAST(short, floatbar->width);
	shape[1].y = 0;
	shape[2].x = WINPR_ASSERTING_INT_CAST(short, shape[1].x - FLOATBAR_BORDER);
	shape[2].y = FLOATBAR_HEIGHT;
	shape[3].x = WINPR_ASSERTING_INT_CAST(short, shape[0].x + FLOATBAR_BORDER);
	shape[3].y = FLOATBAR_HEIGHT;
	shape[4].x = shape[0].x;
	shape[4].y = shape[0].y;
	/* points for drawing the border of the floatbar */
	border[0].x = shape[0].x;
	border[0].y = WINPR_ASSERTING_INT_CAST(short, shape[0].y - 1);
	border[1].x = WINPR_ASSERTING_INT_CAST(short, shape[1].x - 1);
	border[1].y = WINPR_ASSERTING_INT_CAST(short, shape[1].y - 1);
	border[2].x = shape[2].x;
	border[2].y = WINPR_ASSERTING_INT_CAST(short, shape[2].y - 1);
	border[3].x = WINPR_ASSERTING_INT_CAST(short, shape[3].x - 1);
	border[3].y = WINPR_ASSERTING_INT_CAST(short, shape[3].y - 1);
	border[4].x = border[0].x;
	border[4].y = border[0].y;
	/* Fill all pixels with 0 */
	LogDynAndXSetForeground(floatbar->xfc->log, display, shape_gc, 0);
	LogDynAndXFillRectangle(floatbar->xfc->log, display, pmap, shape_gc, 0, 0,
	                        WINPR_ASSERTING_INT_CAST(uint32_t, floatbar->width),
	                        WINPR_ASSERTING_INT_CAST(uint32_t, floatbar->height));
	/* Fill all pixels which should be shown with 1 */
	LogDynAndXSetForeground(floatbar->xfc->log, display, shape_gc, 1);
	XFillPolygon(display, pmap, shape_gc, shape, 5, 0, CoordModeOrigin);
	XShapeCombineMask(display, floatbar->handle, ShapeBounding, 0, 0, pmap, ShapeSet);
	/* draw the float bar */
	LogDynAndXSetForeground(floatbar->xfc->log, display, gc,
	                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_BACKGROUND));
	XFillPolygon(display, floatbar->handle, gc, shape, 4, 0, CoordModeOrigin);
	/* draw an border for the floatbar */
	LogDynAndXSetForeground(floatbar->xfc->log, display, gc,
	                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_BORDER));
	XDrawLines(display, floatbar->handle, gc, border, 5, CoordModeOrigin);
	/* draw the host name connected to (limit to maximum file name) */
	const size_t len = strnlen(floatbar->title, MAX_PATH);
	LogDynAndXSetForeground(floatbar->xfc->log, display, gc,
	                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_FOREGROUND));

	WINPR_ASSERT(len <= INT32_MAX / 2);
	const int fx = floatbar->width / 2 - (int)len * 2;
	if (floatbar->fontSet != NULL)
	{
		XmbDrawString(display, floatbar->handle, floatbar->fontSet, gc, fx, 15, floatbar->title,
		              (int)len);
	}
	else
	{
		XDrawString(display, floatbar->handle, gc, fx, 15, floatbar->title, (int)len);
	}
	LogDynAndXFreeGC(floatbar->xfc->log, display, gc);
	LogDynAndXFreeGC(floatbar->xfc->log, display, shape_gc);
}

static xfFloatbarButton* xf_floatbar_get_button(xfFloatbar* floatbar, Window window)
{
	WINPR_ASSERT(floatbar);
	const size_t size = ARRAYSIZE(floatbar->buttons);

	for (size_t i = 0; i < size; i++)
	{
		xfFloatbarButton* button = floatbar->buttons[i];
		if (button->handle == window)
		{
			return button;
		}
	}

	return NULL;
}

static void xf_floatbar_button_update_positon(xfFloatbar* floatbar)
{
	xfFloatbarButton* button = NULL;
	WINPR_ASSERT(floatbar);
	xfContext* xfc = floatbar->xfc;
	const size_t size = ARRAYSIZE(floatbar->buttons);

	for (size_t i = 0; i < size; i++)
	{
		button = floatbar->buttons[i];

		switch (button->type)
		{
			case XF_FLOATBAR_BUTTON_CLOSE:
				button->x =
				    floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * button->type;
				break;

			case XF_FLOATBAR_BUTTON_RESTORE:
				button->x =
				    floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * button->type;
				break;

			case XF_FLOATBAR_BUTTON_MINIMIZE:
				button->x =
				    floatbar->width - FLOATBAR_BORDER - FLOATBAR_BUTTON_WIDTH * button->type;
				break;

			default:
				break;
		}

		WINPR_ASSERT(xfc);
		WINPR_ASSERT(xfc->display);
		LogDynAndXMoveWindow(xfc->log, xfc->display, button->handle, button->x, button->y);
		xf_floatbar_event_expose(floatbar);
	}
}

static void xf_floatbar_button_event_expose(xfFloatbar* floatbar, Window window)
{
	xfFloatbarButton* button = xf_floatbar_get_button(floatbar, window);
	static unsigned char* bits;
	GC gc = NULL;
	Pixmap pattern = 0;
	xfContext* xfc = floatbar->xfc;

	if (!button)
		return;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);
	WINPR_ASSERT(xfc->window);

	gc = LogDynAndXCreateGC(xfc->log, xfc->display, button->handle, 0, 0);
	floatbar = xfc->window->floatbar;
	WINPR_ASSERT(floatbar);

	switch (button->type)
	{
		case XF_FLOATBAR_BUTTON_CLOSE:
			bits = close_bits;
			break;

		case XF_FLOATBAR_BUTTON_RESTORE:
			bits = restore_bits;
			break;

		case XF_FLOATBAR_BUTTON_MINIMIZE:
			bits = minimize_bits;
			break;

		case XF_FLOATBAR_BUTTON_LOCKED:
			if (floatbar->locked)
				bits = lock_bits;
			else
				bits = unlock_bits;

			break;

		default:
			break;
	}

	pattern = XCreateBitmapFromData(xfc->display, button->handle, (const char*)bits,
	                                FLOATBAR_BUTTON_WIDTH, FLOATBAR_BUTTON_WIDTH);

	if (!(button->focus))
		LogDynAndXSetForeground(floatbar->xfc->log, xfc->display, gc,
		                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_BACKGROUND));
	else
		LogDynAndXSetForeground(floatbar->xfc->log, xfc->display, gc,
		                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_BORDER));

	LogDynAndXSetBackground(xfc->log, xfc->display, gc,
	                        xf_floatbar_get_color(floatbar, FLOATBAR_COLOR_FOREGROUND));
	XCopyPlane(xfc->display, pattern, button->handle, gc, 0, 0, FLOATBAR_BUTTON_WIDTH,
	           FLOATBAR_BUTTON_WIDTH, 0, 0, 1);
	LogDynAndXFreePixmap(xfc->log, xfc->display, pattern);
	LogDynAndXFreeGC(xfc->log, xfc->display, gc);
}

static void xf_floatbar_button_event_buttonpress(xfFloatbar* floatbar, const XButtonEvent* event)
{
	WINPR_ASSERT(event);
	xfFloatbarButton* button = xf_floatbar_get_button(floatbar, event->window);

	if (button)
		button->clicked = TRUE;
}

static void xf_floatbar_button_event_buttonrelease(xfFloatbar* floatbar, const XButtonEvent* event)
{
	xfFloatbarButton* button = NULL;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	button = xf_floatbar_get_button(floatbar, event->window);

	if (button)
	{
		if (button->clicked)
			button->onclick(floatbar);
		button->clicked = FALSE;
	}
}

static void xf_floatbar_event_buttonpress(xfFloatbar* floatbar, const XButtonEvent* event)
{
	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	switch (event->button)
	{
		case Button1:
			if (event->x <= FLOATBAR_BORDER)
				floatbar->mode = XF_FLOATBAR_MODE_RESIZE_LEFT;
			else if (event->x >= (floatbar->width - FLOATBAR_BORDER))
				floatbar->mode = XF_FLOATBAR_MODE_RESIZE_RIGHT;
			else
				floatbar->mode = XF_FLOATBAR_MODE_DRAGGING;

			break;

		default:
			break;
	}
}

static void xf_floatbar_event_buttonrelease(xfFloatbar* floatbar, const XButtonEvent* event)
{
	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	switch (event->button)
	{
		case Button1:
			floatbar->mode = XF_FLOATBAR_MODE_NONE;
			break;

		default:
			break;
	}
}

static void xf_floatbar_resize(xfFloatbar* floatbar, const XMotionEvent* event)
{
	int x = 0;
	int width = 0;
	int movement = 0;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	xfContext* xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);

	/* calculate movement which happened on the root window */
	movement = event->x_root - floatbar->last_motion_x_root;

	/* set x and width depending if movement happens on the left or right  */
	if (floatbar->mode == XF_FLOATBAR_MODE_RESIZE_LEFT)
	{
		x = floatbar->x + movement;
		width = floatbar->width + movement * -1;
	}
	else
	{
		x = floatbar->x;
		width = floatbar->width + movement;
	}

	/* only resize and move window if still above minimum width */
	if (FLOATBAR_MIN_WIDTH < width)
	{
		LogDynAndXMoveResizeWindow(xfc->log, xfc->display, floatbar->handle, x, 0,
		                           WINPR_ASSERTING_INT_CAST(uint32_t, width),
		                           WINPR_ASSERTING_INT_CAST(uint32_t, floatbar->height));
		floatbar->x = x;
		floatbar->width = width;
	}
}

static void xf_floatbar_dragging(xfFloatbar* floatbar, const XMotionEvent* event)
{
	int x = 0;
	int movement = 0;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);
	xfContext* xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->window);
	WINPR_ASSERT(xfc->display);

	/* calculate movement and new x position */
	movement = event->x_root - floatbar->last_motion_x_root;
	x = floatbar->x + movement;

	/* do nothing if floatbar would be moved out of the window */
	if (x < 0 || (x + floatbar->width) > xfc->window->width)
		return;

	/* move window to new x position */
	LogDynAndXMoveWindow(xfc->log, xfc->display, floatbar->handle, x, 0);
	/* update struct values for the next event */
	floatbar->last_motion_x_root = floatbar->last_motion_x_root + movement;
	floatbar->x = x;
}

static void xf_floatbar_event_motionnotify(xfFloatbar* floatbar, const XMotionEvent* event)
{
	int mode = 0;
	Cursor cursor = 0;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	xfContext* xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->display);

	mode = floatbar->mode;
	cursor = XCreateFontCursor(xfc->display, XC_arrow);

	if ((event->state & Button1Mask) && (mode > XF_FLOATBAR_MODE_DRAGGING))
	{
		xf_floatbar_resize(floatbar, event);
	}
	else if ((event->state & Button1Mask) && (mode == XF_FLOATBAR_MODE_DRAGGING))
	{
		xf_floatbar_dragging(floatbar, event);
	}
	else
	{
		if (event->x <= FLOATBAR_BORDER || event->x >= floatbar->width - FLOATBAR_BORDER)
			cursor = XCreateFontCursor(xfc->display, XC_sb_h_double_arrow);
	}

	XDefineCursor(xfc->display, xfc->window->handle, cursor);
	XFreeCursor(xfc->display, cursor);
	floatbar->last_motion_x_root = event->x_root;
}

static void xf_floatbar_button_event_focusin(xfFloatbar* floatbar, const XAnyEvent* event)
{
	xfFloatbarButton* button = NULL;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	button = xf_floatbar_get_button(floatbar, event->window);

	if (button)
	{
		button->focus = TRUE;
		xf_floatbar_button_event_expose(floatbar, event->window);
	}
}

static void xf_floatbar_button_event_focusout(xfFloatbar* floatbar, const XAnyEvent* event)
{
	xfFloatbarButton* button = NULL;

	WINPR_ASSERT(floatbar);
	WINPR_ASSERT(event);

	button = xf_floatbar_get_button(floatbar, event->window);

	if (button)
	{
		button->focus = FALSE;
		xf_floatbar_button_event_expose(floatbar, event->window);
	}
}

static void xf_floatbar_event_focusout(xfFloatbar* floatbar)
{
	WINPR_ASSERT(floatbar);
	xfContext* xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);

	if (xfc->pointer)
	{
		WINPR_ASSERT(xfc->window);
		WINPR_ASSERT(xfc->pointer);
		XDefineCursor(xfc->display, xfc->window->handle, xfc->pointer->cursor);
	}
}

BOOL xf_floatbar_check_event(xfFloatbar* floatbar, const XEvent* event)
{
	if (!floatbar || !floatbar->xfc || !event)
		return FALSE;

	if (!floatbar->created)
		return FALSE;

	if (event->xany.window == floatbar->handle)
		return TRUE;

	size_t size = ARRAYSIZE(floatbar->buttons);

	for (size_t i = 0; i < size; i++)
	{
		const xfFloatbarButton* button = floatbar->buttons[i];

		if (event->xany.window == button->handle)
			return TRUE;
	}

	return FALSE;
}

BOOL xf_floatbar_event_process(xfFloatbar* floatbar, const XEvent* event)
{
	if (!floatbar || !floatbar->xfc || !event)
		return FALSE;

	if (!floatbar->created)
		return FALSE;

	switch (event->type)
	{
		case Expose:
			if (event->xexpose.window == floatbar->handle)
				xf_floatbar_event_expose(floatbar);
			else
				xf_floatbar_button_event_expose(floatbar, event->xexpose.window);

			break;

		case MotionNotify:
			xf_floatbar_event_motionnotify(floatbar, &event->xmotion);
			break;

		case ButtonPress:
			if (event->xany.window == floatbar->handle)
				xf_floatbar_event_buttonpress(floatbar, &event->xbutton);
			else
				xf_floatbar_button_event_buttonpress(floatbar, &event->xbutton);

			break;

		case ButtonRelease:
			if (event->xany.window == floatbar->handle)
				xf_floatbar_event_buttonrelease(floatbar, &event->xbutton);
			else
				xf_floatbar_button_event_buttonrelease(floatbar, &event->xbutton);

			break;

		case EnterNotify:
		case FocusIn:
			if (event->xany.window != floatbar->handle)
				xf_floatbar_button_event_focusin(floatbar, &event->xany);

			break;

		case LeaveNotify:
		case FocusOut:
			if (event->xany.window == floatbar->handle)
				xf_floatbar_event_focusout(floatbar);
			else
				xf_floatbar_button_event_focusout(floatbar, &event->xany);

			break;

		case ConfigureNotify:
			if (event->xany.window == floatbar->handle)
				xf_floatbar_button_update_positon(floatbar);

			break;

		case PropertyNotify:
			if (event->xany.window == floatbar->handle)
				xf_floatbar_button_update_positon(floatbar);

			break;

		default:
			break;
	}

	return floatbar->handle == event->xany.window;
}

static void xf_floatbar_button_free(xfContext* xfc, xfFloatbarButton* button)
{
	if (!button)
		return;

	if (button->handle)
	{
		WINPR_ASSERT(xfc);
		WINPR_ASSERT(xfc->display);
		LogDynAndXUnmapWindow(xfc->log, xfc->display, button->handle);
		LogDynAndXDestroyWindow(xfc->log, xfc->display, button->handle);
	}

	free(button);
}

void xf_floatbar_free(xfFloatbar* floatbar)
{
	size_t size = 0;
	xfContext* xfc = NULL;

	if (!floatbar)
		return;

	free(floatbar->title);
	xfc = floatbar->xfc;
	WINPR_ASSERT(xfc);

	size = ARRAYSIZE(floatbar->buttons);

	for (size_t i = 0; i < size; i++)
	{
		xf_floatbar_button_free(xfc, floatbar->buttons[i]);
		floatbar->buttons[i] = NULL;
	}

	if (floatbar->handle)
	{
		WINPR_ASSERT(xfc->display);
		LogDynAndXUnmapWindow(xfc->log, xfc->display, floatbar->handle);
		LogDynAndXDestroyWindow(xfc->log, xfc->display, floatbar->handle);
	}

	free(floatbar);
}

BOOL xf_floatbar_is_locked(xfFloatbar* floatbar)
{
	if (!floatbar)
		return FALSE;
	return floatbar->mode != XF_FLOATBAR_MODE_NONE;
}
