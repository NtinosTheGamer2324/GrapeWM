/*
 * WM_WindowClass.c
 * Per-window decoration, dragging, close button, and event handling.
 */

#include "component/window/WM_WindowHeader.h"
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

typedef struct ManagedWindow {
    Window frame;
    Window title_bar;
    char  *title_string;
    Window client;
    struct ManagedWindow *next;
} ManagedWindow;

static ManagedWindow *managed_windows = NULL;

/* ------------------------------------------------------------------ */
/* Drag state                                                           */
/* ------------------------------------------------------------------ */

static Display *drag_dpy    = NULL;
static Window   drag_frame  = 0;
static bool     dragging    = false;
static int      drag_start_x = 0, drag_start_y = 0;
static int      win_start_x  = 0, win_start_y  = 0;

/* ------------------------------------------------------------------ */
/* Layout constants                                                     */
/* ------------------------------------------------------------------ */

#define TITLEBAR_HEIGHT 20
#define BUTTON_SIZE     16
#define BUTTON_PADDING   4

typedef struct { int x, y, width, height; } ButtonRect;

/* ------------------------------------------------------------------ */
/* Title helpers                                                        */
/* ------------------------------------------------------------------ */

static char *GetWindowTitle(Display *dpy, Window win) {
    Atom net_wm_name  = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8_string  = XInternAtom(dpy, "UTF8_STRING",  False);
    Atom actual_type;
    int  actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, win, net_wm_name, 0, (~0L), False,
                           utf8_string, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop)
        return (char *)prop;   /* caller must XFree */

    char *name = NULL;
    if (XFetchName(dpy, win, &name) && name)
        return name;           /* caller must XFree */

    return NULL;
}

static void DrawTitle(Display *dpy, Window title_bar, const char *title) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, title_bar, &attr)) return;

    int width  = attr.width;
    int height = attr.height;
    int scr    = DefaultScreen(dpy);

    XClearWindow(dpy, title_bar);

    /* --- Xft text --- */
    XftDraw *draw = XftDrawCreate(dpy, title_bar,
                                  DefaultVisual(dpy, scr),
                                  DefaultColormap(dpy, scr));
    if (!draw) return;

    XftFont *font = XftFontOpenName(dpy, scr, "DejaVu Sans-12");
    if (!font) { XftDrawDestroy(draw); return; }

    XRenderColor rc = { 0xc000, 0xc000, 0xc000, 0xffff };
    XftColor color;
    if (!XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                            DefaultColormap(dpy, scr), &rc, &color)) {
        XftFontClose(dpy, font);
        XftDrawDestroy(draw);
        return;
    }

    if (title) {
        int y = (height + font->ascent) / 2 - 2;
        XftDrawStringUtf8(draw, &color, font, 5, y,
                          (XftChar8 *)title, (int)strlen(title));
    }

    XftColorFree(dpy, DefaultVisual(dpy, scr),
                 DefaultColormap(dpy, scr), &color);
    XftFontClose(dpy, font);
    XftDrawDestroy(draw);

    /* --- Close button --- */
    GC gc = XCreateGC(dpy, title_bar, 0, NULL);

    /* Button background */
    XSetForeground(dpy, gc, 0x882222);
    int bx = width - BUTTON_SIZE - BUTTON_PADDING;
    int by = (height - BUTTON_SIZE) / 2;
    XFillRectangle(dpy, title_bar, gc, bx, by, BUTTON_SIZE, BUTTON_SIZE);

    /* X mark */
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    int margin = 4;
    XDrawLine(dpy, title_bar, gc,
              bx + margin,              by + margin,
              bx + BUTTON_SIZE - margin, by + BUTTON_SIZE - margin);
    XDrawLine(dpy, title_bar, gc,
              bx + BUTTON_SIZE - margin, by + margin,
              bx + margin,              by + BUTTON_SIZE - margin);

    XFreeGC(dpy, gc);
    XFlush(dpy);
}

/* ------------------------------------------------------------------ */
/* WM_DELETE_WINDOW                                                     */
/* ------------------------------------------------------------------ */

static void SendCloseMessage(Display *dpy, Window client) {
    Atom wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS",     False);

    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = client;
    ev.xclient.message_type = wm_protocols;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)wm_delete;
    ev.xclient.data.l[1]    = CurrentTime;

    XSendEvent(dpy, client, False, NoEventMask, &ev);
    XFlush(dpy);
}

/* ------------------------------------------------------------------ */
/* ManagedWindow list helpers                                           */
/* ------------------------------------------------------------------ */

static ManagedWindow *FindByFrame(Window frame) {
    for (ManagedWindow *w = managed_windows; w; w = w->next)
        if (w->frame == frame) return w;
    return NULL;
}

static ManagedWindow *FindByTitleBar(Window title_bar) {
    for (ManagedWindow *w = managed_windows; w; w = w->next)
        if (w->title_bar == title_bar) return w;
    return NULL;
}

static ManagedWindow *FindByClient(Window client) {
    for (ManagedWindow *w = managed_windows; w; w = w->next)
        if (w->client == client) return w;
    return NULL;
}

static void RemoveManagedWindow(Display *dpy, ManagedWindow *mw) {
    if (!mw) return;

    // Grab X errors silently during cleanup — window may already be gone
    XSync(dpy, False);

    // Only destroy title bar and frame if they still exist
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, mw->title_bar, &attr)) {
        XUnmapWindow(dpy, mw->title_bar);
        XDestroyWindow(dpy, mw->title_bar);
    }
    if (XGetWindowAttributes(dpy, mw->frame, &attr)) {
        XUnmapWindow(dpy, mw->frame);
        XDestroyWindow(dpy, mw->frame);
    }

    free(mw->title_string);

    ManagedWindow **p = &managed_windows;
    while (*p) {
        if (*p == mw) { *p = mw->next; break; }
        p = &(*p)->next;
    }
    free(mw);
    XSync(dpy, False);
}

/* ------------------------------------------------------------------ */
/* Decoratability check                                                 */
/* ------------------------------------------------------------------ */

static bool HasProperty(Display *dpy, Window win, const char *name) {
    Atom prop = XInternAtom(dpy, name, False);
    Atom actual_type; int fmt; unsigned long n, ba;
    unsigned char *data = NULL;
    int st = XGetWindowProperty(dpy, win, prop, 0, 0, False, AnyPropertyType,
                                &actual_type, &fmt, &n, &ba, &data);
    if (data) XFree(data);
    return (st == Success && actual_type != None);
}

static bool HasMotifNoDecor(Display *dpy, Window win) {
    Atom hint_atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    if (hint_atom == None) return false;

    Atom actual_type; int fmt; unsigned long n, ba;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, hint_atom, 0, 5, False, hint_atom,
                           &actual_type, &fmt, &n, &ba, &data) != Success)
        return false;
    if (!data) return false;

    const unsigned long *hints = (const unsigned long *)data;
    bool no_decor = (hints[0] & (1 << 1)) && (hints[2] == 0);
    XFree(data);
    return no_decor;
}

bool WM_Window_IsDecoratable(Display *dpy, Window win) {
    Atom type_atom    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom dock_atom    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK",    False);
    Atom desktop_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom splash_atom  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH",  False);
    Atom tooltip_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    Atom menu_atom    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU",    False);

    Atom actual_type; int fmt; unsigned long n, ba;
    Atom *props = NULL;

    if (XGetWindowProperty(dpy, win, type_atom, 0, (~0L), False, XA_ATOM,
                           &actual_type, &fmt, &n, &ba,
                           (unsigned char **)&props) == Success && props) {
        for (unsigned long i = 0; i < n; ++i) {
            if (props[i] == dock_atom    || props[i] == desktop_atom ||
                props[i] == splash_atom  || props[i] == tooltip_atom ||
                props[i] == menu_atom) {
                XFree(props);
                return false;
            }
        }
        XFree(props);
    }

    if (HasProperty(dpy, win, "_GTK_FRAME_EXTENTS")) return false;
    if (HasMotifNoDecor(dpy, win))                   return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void WM_Window_MapWindow(Display *dpy, Window win) {
    XMapWindow(dpy, win);
    XFlush(dpy);
}

void WM_Window_ConfigureWindow(Display *dpy, XConfigureRequestEvent *req) {
    XWindowChanges ch = {
        .x            = req->x,
        .y            = req->y,
        .width        = req->width,
        .height       = req->height,
        .border_width = req->border_width,
        .sibling      = req->above,
        .stack_mode   = req->detail,
    };
    XConfigureWindow(dpy, req->window, req->value_mask, &ch);
    XFlush(dpy);
}

Window WM_Window_CreateFrame(Display *dpy, Window client_win) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, client_win, &attr)) return 0;

    // If the client reports a tiny size at map time, check size hints
    int client_w = attr.width;
    int client_h = attr.height;

    if (client_w < 50 || client_h < 50) {
        XSizeHints hints;
        long supplied;
        if (XGetWMNormalHints(dpy, client_win, &hints, &supplied)) {
            if ((hints.flags & PSize) && hints.width > 50 && hints.height > 50) {
                client_w = hints.width;
                client_h = hints.height;
            } else if ((hints.flags & PBaseSize) && hints.base_width > 50) {
                client_w = hints.base_width;
                client_h = hints.base_height;
            } else {
                // Fallback default size
                client_w = 640;
                client_h = 480;
            }
        } else {
            client_w = 640;
            client_h = 480;
        }
        // Resize the client to match
        XResizeWindow(dpy, client_win, client_w, client_h);
    }

    int fx = attr.x - 2;
    int fy = attr.y - 2;
    int fw = client_w + 4;
    int fh = client_h + TITLEBAR_HEIGHT + 4;

    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor bg_col, accent_col;
    XParseColor(dpy, cmap, "#282c34", &bg_col);
    XAllocColor(dpy, cmap, &bg_col);
    XParseColor(dpy, cmap, "#383c3c", &accent_col);
    XAllocColor(dpy, cmap, &accent_col);

    Window frame = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                       fx, fy, fw, fh,
                                       2,
                                       accent_col.pixel,
                                       bg_col.pixel);

    Window title_bar = XCreateSimpleWindow(dpy, frame,
                                           0, 0, fw, TITLEBAR_HEIGHT,
                                           0,
                                           accent_col.pixel,
                                           accent_col.pixel);

    XReparentWindow(dpy, client_win, frame, 2, TITLEBAR_HEIGHT + 2);

    XSelectInput(dpy, frame,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | StructureNotifyMask);
    XSelectInput(dpy, title_bar,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask);
    XSelectInput(dpy, client_win, StructureNotifyMask);

    ManagedWindow *mw = calloc(1, sizeof(ManagedWindow));
    if (!mw) return 0;

    mw->frame     = frame;
    mw->title_bar = title_bar;
    mw->client    = client_win;

    char *title = GetWindowTitle(dpy, client_win);
    if (title) {
        mw->title_string = strdup(title);
        XFree(title);
    }

    mw->next        = managed_windows;
    managed_windows = mw;

    XMapWindow(dpy, frame);
    XMapWindow(dpy, title_bar);
    XMapWindow(dpy, client_win);

    DrawTitle(dpy, title_bar, mw->title_string);
    XFlush(dpy);

    return frame;
}

void WM_Window_HandleEvents(Display *dpy, XEvent *event) {
    if (!dpy || !event) return;

    switch (event->type) {

    case ButtonPress: {
        if (event->xbutton.button != Button1) break;

        Window clicked = event->xbutton.window;

        /* ---- hit-test the title bar first ---- */
        ManagedWindow *mw = FindByTitleBar(clicked);
        if (mw) {
            XWindowAttributes ta;
            if (XGetWindowAttributes(dpy, mw->title_bar, &ta)) {
                ButtonRect btn = {
                    .x      = ta.width - BUTTON_SIZE - BUTTON_PADDING,
                    .y      = (ta.height - BUTTON_SIZE) / 2,
                    .width  = BUTTON_SIZE,
                    .height = BUTTON_SIZE,
                };
                int mx = event->xbutton.x;
                int my = event->xbutton.y;
                if (mx >= btn.x && mx <= btn.x + btn.width &&
                    my >= btn.y && my <= btn.y + btn.height) {
                    SendCloseMessage(dpy, mw->client);
                    return;
                }
            }
            /* start drag */
            XRaiseWindow(dpy, mw->frame);
            drag_frame  = mw->frame;
            drag_dpy    = dpy;
            dragging    = true;
            drag_start_x = event->xbutton.x_root;
            drag_start_y = event->xbutton.y_root;
            XWindowAttributes fa;
            if (XGetWindowAttributes(dpy, drag_frame, &fa)) {
                win_start_x = fa.x;
                win_start_y = fa.y;
            }
            return;
        }

        /* ---- click landed somewhere inside a frame ---- */
        Window root_ret, parent_ret;
        Window *children = NULL;
        unsigned int nch;
        if (XQueryTree(dpy, clicked, &root_ret, &parent_ret, &children, &nch)) {
            if (children) XFree(children);
            if (FindByFrame(parent_ret)) {
                XRaiseWindow(dpy, parent_ret);
                drag_frame  = parent_ret;
                drag_dpy    = dpy;
                dragging    = true;
                drag_start_x = event->xbutton.x_root;
                drag_start_y = event->xbutton.y_root;
                XWindowAttributes fa;
                if (XGetWindowAttributes(dpy, drag_frame, &fa)) {
                    win_start_x = fa.x;
                    win_start_y = fa.y;
                }
            }
        }
        break;
    }

    case MotionNotify: {
        if (dragging && drag_dpy && drag_frame) {
            int dx = event->xmotion.x_root - drag_start_x;
            int dy = event->xmotion.y_root - drag_start_y;
            XMoveWindow(drag_dpy, drag_frame,
                        win_start_x + dx, win_start_y + dy);
            XFlush(drag_dpy);
        }
        break;
    }

    case ButtonRelease: {
        if (event->xbutton.button == Button1) {
            dragging   = false;
            drag_dpy   = NULL;
            drag_frame = 0;
        }
        break;
    }

    case Expose: {
        if (event->xexpose.count != 0) break;
        ManagedWindow *mw = FindByTitleBar(event->xexpose.window);
        if (mw) DrawTitle(dpy, mw->title_bar, mw->title_string);
        break;
    }

    case DestroyNotify: {
        Window destroyed = event->xdestroywindow.window;

        // Check if it's a client window
        ManagedWindow *mw = FindByClient(destroyed);
        if (mw) {
            RemoveManagedWindow(dpy, mw);
            break;
        }

        // Check if it's a frame or titlebar (shouldn't happen but handle it)
        mw = FindByFrame(destroyed);
        if (!mw) {
            for (ManagedWindow *w = managed_windows; w; w = w->next) {
                if (w->title_bar == destroyed) { mw = w; break; }
            }
        }
        if (mw) RemoveManagedWindow(dpy, mw);
        break;
    }

    case UnmapNotify: {
        Window uw = event->xunmap.window;
        ManagedWindow *mw = FindByClient(uw);
        if (!mw) break;
    
        // Always clean up on unmap — don't wait for WM_STATE
        // Some apps (dialogs, pcmanfm) never set WithdrawnState
        // but still expect the frame to be gone
        XWindowAttributes attr;
        if (XGetWindowAttributes(dpy, uw, &attr)) {
            // Window still exists but is unmapping — check WM_STATE
            Atom wm_state = XInternAtom(dpy, "WM_STATE", True);
            if (wm_state != None) {
                Atom at; int af; unsigned long n, ba;
                unsigned char *prop = NULL;
                if (XGetWindowProperty(dpy, uw, wm_state, 0, 2, False,
                                       AnyPropertyType, &at, &af, &n, &ba,
                                       &prop) == Success && prop) {
                    long state = *(long *)prop;
                    XFree(prop);
                    if (state == WithdrawnState)
                        RemoveManagedWindow(dpy, mw);
                }
            }
        } else {
            // XGetWindowAttributes failed — window is already gone, clean up
            RemoveManagedWindow(dpy, mw);
        }
        break;
    }

    default:
        break;
    }
}