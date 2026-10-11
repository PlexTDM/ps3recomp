/*
 * The mouse of the window the frame is presented in, for a port that is
 * played with keyboard and mouse.
 *
 * Motion comes as raw input, so the pointer's acceleration and the screen's
 * edges are out of it. While the port asks for it and the window has the
 * focus, the cursor is hidden and held inside the window; it is let go the
 * moment the focus is.
 *
 * Windows: the D3D12 and Vulkan engines hand their window and its messages
 * to rsx_host_mouse_attach / rsx_host_mouse_message. macOS: the Metal
 * backend has the window and implements the calls itself; motion is the
 * events' own (points, with the system's tracking speed in them, as every
 * pointing device there reports), and Escape never closed that window.
 * Elsewhere the calls do nothing and there is never any motion.
 */
#ifndef PS3RECOMP_RSX_HOST_MOUSE_H
#define PS3RECOMP_RSX_HOST_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hide the cursor and keep it in the window while it has the focus (on), or
 * leave the cursor alone (off, the default). Any thread. */
void rsx_host_mouse_capture(int on);
/* Asked for and in force now: the window is in front. */
int rsx_host_mouse_captured(void);
/* The motion since the last call, in the mouse's own counts (+x right, +y
 * towards the player). Only motion while captured is counted. Any thread. */
void rsx_host_mouse_take_motion(int* dx, int* dy);
/* The wheel's notches since the last call (+ away from the player). */
int rsx_host_mouse_take_wheel(void);

/* Escape closes the window unless a port says otherwise (it wants the key). */
void rsx_host_window_escape_closes(int on);

/* For the engines' window code. hwnd is an HWND; a nonzero return means the
 * message was dealt with and *result is what the window procedure returns. */
void rsx_host_mouse_attach(void* hwnd);
int rsx_host_mouse_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HOST_MOUSE_H */
