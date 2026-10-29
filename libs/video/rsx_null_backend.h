/*
 * ps3recomp - Null RSX Backend (headless software rasteriser)
 *
 * Minimal graphics backend that:
 * - Renders into a host-memory framebuffer (no window, no GPU)
 * - Clears to the RSX clear color on each frame
 * - Flat-shades triangles and presents the centre pixel for readback
 *
 * This proves the RSX command processor is receiving commands and the game
 * loop is running, and it works on a CI runner with no display or GPU driver.
 * Use the Vulkan backend for actual rendering.
 */

#if !defined(PS3RECOMP_RSX_NULL_BACKEND_H)
#define PS3RECOMP_RSX_NULL_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create and register the null backend.
 * Allocates the framebuffer of the specified size and starts accepting RSX
 * commands. Returns 0 on success. */
int rsx_null_backend_init(u32 width, u32 height, const char* title);

/* Shut down the null backend and free the framebuffer. */
void rsx_null_backend_shutdown(void);

/* No event queue and no window: always returns 0. Kept so the frame clock can
 * call every backend through the same entry points. */
int rsx_null_backend_pump_messages(void);

/* --- headless software backend -----------------------------------------
 * The backend renders into a host-memory framebuffer, which makes it usable
 * on a CI runner with no display and no GPU. These are the test hooks a host
 * uses to assert what actually came out of the guest command stream.
 * ---------------------------------------------------------------------- */

/* Copy the framebuffer's centre pixel out as the presented frame. */
void rsx_null_backend_present(void);

/* The clear colour most recently set via NV4097_SET_COLOR_CLEAR_VALUE, in the
 * RSX's native ARGB8888. Lets a host assert the command stream arrived. */
u32 rsx_null_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing has
 * been presented yet. */
u32 rsx_null_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs:
 * always 0 here, this backend has no shader path. */
u32 rsx_null_backend_guest_draws(void);

#ifdef __cplusplus
}
#endif
#endif
