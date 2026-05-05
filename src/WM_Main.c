/*
 * wm_main.c — GRAPE Window Manager entry point
 * Copyright © 2025 NewTechnologies LLC.
 * Licensed under the GNU General Public License v3.0 or later.
 */

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "component/cursor/WM_CursorHeader.h"
#include "windows/WM_WindowHandlerHeader.h"

#define PID_FILE "/tmp/grape.pid"

volatile sig_atomic_t running      = 1;
Display              *signal_display = NULL;

static void handle_exit_signal(int sig) {
    (void)sig;
    running = 0;

    if (signal_display) {
        Window root = DefaultRootWindow(signal_display);
        XEvent ev   = {0};
        ev.xclient.type         = ClientMessage;
        ev.xclient.window       = root;
        ev.xclient.message_type =
            XInternAtom(signal_display, "GRAPE_EXIT", False);
        ev.xclient.format = 32;
        XSendEvent(signal_display, root, False, 0, &ev);
        XFlush(signal_display);
    }
}

int main(int argc, char **argv) {
    /* --close: signal a running instance to quit */
    if (argc > 1 && strcmp(argv[1], "--close") == 0) {
        FILE *pf = fopen(PID_FILE, "r");
        if (!pf) {
            fprintf(stderr, "GRAPE is not running (pid file missing)\n");
            return 1;
        }
        pid_t pid = 0;
        if (fscanf(pf, "%d", &pid) != 1) {
            fclose(pf);
            fprintf(stderr, "Failed to read pid from %s\n", PID_FILE);
            return 1;
        }
        fclose(pf);
        if (kill(pid, SIGUSR1) == 0) {
            printf("Sent close signal to GRAPE (pid %d)\n", pid);
            return 0;
        }
        perror("Failed to send signal to GRAPE");
        return 1;
    }

    /* Write PID file */
    {
        FILE *pf = fopen(PID_FILE, "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
        else      fprintf(stderr, "Warning: could not write %s\n", PID_FILE);
    }

    /* Signal handler */
    struct sigaction sa = { .sa_handler = handle_exit_signal, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* Open display */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        remove(PID_FILE);
        return 1;
    }
    signal_display = dpy;

    /* Font (loaded once here to verify Xft is working) */
    int      scr  = DefaultScreen(dpy);
    XftFont *font = XftFontOpenName(dpy, scr, "DejaVu Sans-12");
    if (!font) {
        fprintf(stderr, "Failed to open Xft font\n");
        XCloseDisplay(dpy);
        remove(PID_FILE);
        return 1;
    }

    /* Cursor */
    if (!cursor_init(dpy)) {
        fprintf(stderr, "Failed to initialize cursor\n");
        XftFontClose(dpy, font);
        XCloseDisplay(dpy);
        remove(PID_FILE);
        return 1;
    }
    cursor_show(dpy);

    /* Window manager */
    if (!WM_WindowHandler_Init(dpy)) {
        fprintf(stderr, "Failed to initialize window handler\n");
        cursor_hide(dpy);
        XftFontClose(dpy, font);
        XCloseDisplay(dpy);
        remove(PID_FILE);
        return 1;
    }

    printf("GRAPE Window Manager started (PID: %d)\n", getpid());

    WM_WindowHandler_RunLoop(dpy);   /* blocks until exit */

    /* Cleanup */
    cursor_hide(dpy);
    XftFontClose(dpy, font);
    XCloseDisplay(dpy);
    remove(PID_FILE);

    printf("GRAPE Window Manager exited cleanly\n");
    return 0;
}