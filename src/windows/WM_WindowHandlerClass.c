/*
 * WM_WindowHandlerClass.c
 * Manages the list of client/frame pairs, the X event loop,
 * and _NET_CLIENT_LIST updates.
 */

#include "windows/WM_WindowHandlerHeader.h"
#include "component/window/WM_WindowHeader.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>

extern volatile sig_atomic_t running;

static Display *display = NULL;
static int screen;

typedef struct {
    Window client;
    Window frame;
} ClientFramePair;

static ClientFramePair *managed_clients = NULL;
static size_t client_count = 0;

static Window fwm_own_window = None;

void WM_WindowHandler_SetWMWindow(Window win) {
    fwm_own_window = win;
}

/* Forward declaration needed by the error handler */
static void RemoveClient(Window win);

static int XErrorHandlerFunc(Display *dpy, XErrorEvent *error) {
    char err_text[1024];
    XGetErrorText(dpy, error->error_code, err_text, sizeof(err_text));
    fprintf(stderr,
            "[GRAPE] X Error — request: %d, error: %s (code %d), resource: 0x%lx\n",
            error->request_code, err_text, error->error_code, error->resourceid);

    if (error->error_code == BadWindow)
        RemoveClient((Window)error->resourceid);

    return 0;
}

/* ------------------------------------------------------------------ */
/* _NET_CLIENT_LIST helpers                                             */
/* ------------------------------------------------------------------ */

static void UpdateClientList(void) {
    if (!display) return;
    Atom net_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    Window root = RootWindow(display, screen);

    if (client_count > 0) {
        Window *wins = malloc(sizeof(Window) * client_count);
        if (!wins) return;
        for (size_t i = 0; i < client_count; ++i)
            wins[i] = managed_clients[i].client;
        XChangeProperty(display, root, net_client_list, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)wins, (int)client_count);
        free(wins);
    } else {
        XDeleteProperty(display, root, net_client_list);
    }
    XFlush(display);
}

/* ------------------------------------------------------------------ */
/* Lookup helpers                                                       */
/* ------------------------------------------------------------------ */

static Window FindFrame(Window client) {
    for (size_t i = 0; i < client_count; ++i)
        if (managed_clients[i].client == client)
            return managed_clients[i].frame;
    return None;
}

static ssize_t FindClientIndex(Window win) {
    for (size_t i = 0; i < client_count; ++i)
        if (managed_clients[i].client == win || managed_clients[i].frame == win)
            return (ssize_t)i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Add / remove                                                         */
/* ------------------------------------------------------------------ */

static bool WindowExists(Display *dpy, Window win) {
    XWindowAttributes attr;
    return XGetWindowAttributes(dpy, win, &attr) != 0;
}

static bool AddClient(Window client, Window frame) {
    if (client == fwm_own_window) return false;
    if (FindFrame(client) != None) return false;   /* already tracked */

    ClientFramePair *list =
        realloc(managed_clients, sizeof(ClientFramePair) * (client_count + 1));
    if (!list) {
        fprintf(stderr, "[GRAPE] OOM adding client 0x%lx\n", client);
        return false;
    }

    managed_clients = list;
    managed_clients[client_count].client = client;
    managed_clients[client_count].frame  = frame;
    ++client_count;
    UpdateClientList();
    return true;
}

static void RemoveClient(Window win) {
    if (!managed_clients || client_count == 0) return;

    ssize_t idx = FindClientIndex(win);
    if (idx < 0) return;

    /* Destroy frame if it still exists.
       WM_WindowClass already destroys its own ManagedWindow entry
       (title bar + frame) via DestroyNotify → WM_Window_RemoveManagedWindow.
       We only need to guard against the case where the frame was stored here
       but WM_WindowClass never saw a DestroyNotify for it. */
    Window frame = managed_clients[idx].frame;
    if (frame != None && WindowExists(display, frame)) {
        XUnmapWindow(display, frame);
        XDestroyWindow(display, frame);
    }

    /* Shift remaining entries left */
    for (size_t j = (size_t)idx; j < client_count - 1; ++j)
        managed_clients[j] = managed_clients[j + 1];
    --client_count;

    if (client_count > 0) {
        ClientFramePair *list =
            realloc(managed_clients, sizeof(ClientFramePair) * client_count);
        if (list) managed_clients = list;
    } else {
        free(managed_clients);
        managed_clients = NULL;
    }

    UpdateClientList();
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool WM_WindowHandler_Init(Display *dpy) {
    if (!dpy) return false;
    display = dpy;
    screen  = DefaultScreen(display);

    XSetErrorHandler(XErrorHandlerFunc);

    XSelectInput(display, RootWindow(display, screen),
                 SubstructureRedirectMask | SubstructureNotifyMask | FocusChangeMask);
    XSync(display, False);
    return true;
}

void WM_WindowHandler_Cleanup(void) {
    if (managed_clients) {
        for (size_t i = 0; i < client_count; ++i) {
            Window f = managed_clients[i].frame;
            if (f != None && WindowExists(display, f)) {
                XUnmapWindow(display, f);
                XDestroyWindow(display, f);
            }
        }
        free(managed_clients);
        managed_clients = NULL;
        client_count    = 0;
    }
    if (display) {
        XSetErrorHandler(NULL);
        XFlush(display);
    }
}

void WM_WindowHandler_RunLoop(Display *dpy) {
    if (!dpy) {
        fprintf(stderr, "[GRAPE] RunLoop: NULL display\n");
        return;
    }
    display = dpy;   /* keep in sync (Init already set this, but be safe) */

    XEvent event;

    /* Use the same atom name that main() sends: "GRAPE_EXIT" */
    Atom exit_atom      = XInternAtom(display, "GRAPE_EXIT",        False);
    Atom net_close_atom = XInternAtom(display, "_NET_CLOSE_WINDOW", False);
    Atom wm_protocols   = XInternAtom(display, "WM_PROTOCOLS",      False);
    Atom wm_delete      = XInternAtom(display, "WM_DELETE_WINDOW",  False);

    Window focused_window = None;

    while (running) {
        XNextEvent(display, &event);

        /* ---- shutdown message ---- */
        if (event.type == ClientMessage &&
            event.xclient.message_type == exit_atom) {
            printf("[GRAPE] Exit message received — shutting down.\n");
            break;
        }

        /* ---- polite close request from external source ---- */
        if (event.type == ClientMessage &&
            event.xclient.message_type == net_close_atom) {
            Window target = event.xclient.window;
            XWindowAttributes attr;
            if (!XGetWindowAttributes(display, target, &attr) ||
                attr.map_state == IsUnmapped)
                continue;

            XEvent msg = {0};
            msg.xclient.type         = ClientMessage;
            msg.xclient.message_type = wm_protocols;
            msg.xclient.display      = display;
            msg.xclient.window       = target;
            msg.xclient.format       = 32;
            msg.xclient.data.l[0]    = (long)wm_delete;
            msg.xclient.data.l[1]    = CurrentTime;

            if (!XSendEvent(display, target, False, NoEventMask, &msg))
                fprintf(stderr, "[GRAPE] Warning: WM_DELETE_WINDOW to 0x%lx failed\n",
                        target);
            XFlush(display);
            continue;
        }

        /* ---- delegate to the decoration/drag layer first ---- */
        WM_Window_HandleEvents(display, &event);

        /* ---- structural events ---- */
        switch (event.type) {

        case MapRequest: {
            Window client = event.xmaprequest.window;
            if (!WindowExists(display, client)) break;

            if (WM_Window_IsDecoratable(display, client)) {
                if (FindFrame(client) == None) {
                    Window frame = WM_Window_CreateFrame(display, client);
                    if (frame) {
                        WM_Window_MapWindow(display, frame);
                        AddClient(client, frame);
                    }
                } else {
                    /* Frame exists — just make sure client is mapped */
                    XWindowAttributes attr;
                    if (XGetWindowAttributes(display, client, &attr) &&
                        attr.map_state == IsUnmapped)
                        WM_Window_MapWindow(display, client);
                }
            } else {
                AddClient(client, None);
                WM_Window_MapWindow(display, client);
            }

            XSetInputFocus(display, client, RevertToPointerRoot, CurrentTime);
            focused_window = client;
            break;
        }

        case ConfigureRequest: {
            /* Only honour requests for windows we manage */
            if (FindClientIndex(event.xconfigurerequest.window) >= 0)
                WM_Window_ConfigureWindow(display, &event.xconfigurerequest);
            break;
        }

        case DestroyNotify: {
            Window destroyed = event.xdestroywindow.window;
            /*
             * WM_Window_HandleEvents already cleaned up the ManagedWindow
             * (title bar, frame, linked list node) for the *client* window.
             * RemoveClient here removes the ClientFramePair entry and updates
             * _NET_CLIENT_LIST.  Passing the client window is fine; if we
             * receive a destroy for the frame window we also want to clean up.
             */
            RemoveClient(destroyed);

            if (focused_window == destroyed) {
                XSetInputFocus(display, PointerRoot, RevertToPointerRoot, CurrentTime);
                focused_window = None;
            }
            break;
        }

        case FocusIn:
            focused_window = event.xfocus.window;
            break;

        case FocusOut:
            if (focused_window == event.xfocus.window)
                focused_window = None;
            break;

        default:
            break;
        }
    }

    WM_WindowHandler_Cleanup();
}