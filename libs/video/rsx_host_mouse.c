/*
 * The presenting window's mouse: raw motion, the wheel, and the cursor's
 * capture. See rsx_host_mouse.h.
 */
#include "rsx_host_mouse.h"


void rsx_host_mouse_capture(int on) { (void)on; }
int rsx_host_mouse_captured(void) { return 0; }
void rsx_host_mouse_take_motion(int* dx, int* dy) { *dx = 0; *dy = 0; }
int rsx_host_mouse_take_wheel(void) { return 0; }
void rsx_host_window_escape_closes(int on) { (void)on; }
void rsx_host_mouse_attach(void* hwnd) { (void)hwnd; }
int rsx_host_mouse_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp; (void)result;
    return 0;
}

