/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXC6_GLUE_H
#define NXC6_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The FACE_LAYOUT a port hands to the declare boundary. Values mirror
 * nxinput_gptk_face_layout on purpose (0 auto, 1 modern, 2 retro), stated
 * here so the SDL-side glue needs no GPTK header. */
#define NXC6_FACE_LAYOUT_AUTO 0
#define NXC6_FACE_LAYOUT_MODERN 1
#define NXC6_FACE_LAYOUT_RETRO 2

/* Called from MaybeAddDevice() immediately before SDL_PrivateJoystickAdded().
 * Returns 1 when SDL may announce the device and 0 when it must not.
 *
 * 1 is also returned when the port declared nothing, so an unadopted game
 * sees stock SDL. A refusal is only ever the result of an explicit C3
 * decision that failed.
 *
 * `name` (the _named variant) is the device name SDL itself read; it enters
 * the receipt as sanitized EVIDENCE only and never selects a mapping, a
 * domain or a layout. The unnamed symbol stays for 0.9.0 call sites. */
int nxc6_admit_before_announce_named(int instance_id, const char *guid_string,
                                     const char *devpath, const char *name);
int nxc6_admit_before_announce(int instance_id, const char *guid_string,
                               const char *devpath);

/* Called when SDL removes a device, so the instance's decision dies with it
 * and the next device to take that id inherits nothing. */
void nxc6_forget(int instance_id);

/* Declares the port's pinned NXCONTROLLER_PROFILES/1 bundle (authority 3)
 * to the seam when the launcher did not, selected by the port's
 * FACE_LAYOUT:
 *
 *   auto   -> <gamedir>/controllers.nxb (the invariant base, exactly as
 *             0.9.0 declared);
 *   modern -> <gamedir>/controllers-modern.nxb;
 *   retro  -> <gamedir>/controllers-retro.nxb.
 *
 * FACE_LAYOUT never outranks the live authorities: whatever is declared
 * here is still only step 3 of the sovereign order. The file must be a
 * regular, non-symlink file; a missing or unsafe variant simply leaves the
 * ladder without step 3 (returns 0, benign). An environment the launcher or
 * owner already set is never overwritten. Call once, before the joystick
 * subsystem initialises.
 *
 * Returns 1 when a bundle is declared (by this call or already), 0 when the
 * port ships none (benign -- the caller MUST NOT treat it as an error),
 * -1 on invalid input or a failed setenv. */
int nxc6_declare_port_bundle_for_layout(const char *gamedir, int face_layout);

/* Legacy 0.9.0 spelling; means FACE_LAYOUT auto. */
int nxc6_declare_port_bundle(const char *gamedir);

#ifdef __cplusplus
}
#endif

#endif /* NXC6_GLUE_H */
