#ifndef WM_WINDOW_HANDLER_HEADER_H
#define WM_WINDOW_HANDLER_HEADER_H

#include <X11/Xlib.h>
#include <stdbool.h>

bool WM_WindowHandler_Init(Display *dpy);
void WM_WindowHandler_SetWMWindow(Window win);
void WM_WindowHandler_RunLoop(Display *dpy);  // fixed: takes Display*
void WM_WindowHandler_Cleanup(void);

#endif // WM_WINDOW_HANDLER_HEADER_H