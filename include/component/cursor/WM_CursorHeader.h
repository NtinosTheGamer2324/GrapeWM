#ifndef WM_CURSORHEADER_H
#define WM_CURSORHEADER_H

#include <X11/Xlib.h>
#include <stdbool.h>

bool cursor_init(Display *dpy);
void cursor_show(Display *dpy);
void cursor_hide(Display *dpy);

#endif // WM_CURSORHEADER_H