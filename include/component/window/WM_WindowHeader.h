#ifndef WM_WINDOW_HEADER_H
#define WM_WINDOW_HEADER_H

#include <X11/Xlib.h>
#include <stdbool.h>

// Map a window normally
void WM_Window_MapWindow(Display *dpy, Window win);

// Configure window from a ConfigureRequest event
void WM_Window_ConfigureWindow(Display *dpy, XConfigureRequestEvent *req);

// Create a decorated frame window for the client window; returns frame handle
Window WM_Window_CreateFrame(Display *dpy, Window client_win);

// Handle all relevant X events (dragging, close button, expose, etc.)
void WM_Window_HandleEvents(Display *dpy, XEvent *event);

// Returns true if this window should receive decorations
bool WM_Window_IsDecoratable(Display *dpy, Window win);

#endif // WM_WINDOW_HEADER_H