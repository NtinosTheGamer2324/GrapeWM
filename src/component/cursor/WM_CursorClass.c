#include "component/cursor/WM_CursorHeader.h"
#include <X11/cursorfont.h>

static Cursor cursor = 0;

bool cursor_init(Display *dpy) {
    if (!dpy) return false;
    cursor = XCreateFontCursor(dpy, XC_left_ptr);
    return cursor != 0;
}

void cursor_show(Display *dpy) {
    if (!dpy || !cursor) return;
    Window root = DefaultRootWindow(dpy);
    XDefineCursor(dpy, root, cursor);
    XFlush(dpy);
}

void cursor_hide(Display *dpy) {
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy);
    XUndefineCursor(dpy, root);
    if (cursor) {
        XFreeCursor(dpy, cursor);
        cursor = 0;
    }
    XFlush(dpy);
}