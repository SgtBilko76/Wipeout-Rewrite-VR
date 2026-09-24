#ifndef RENDER_VR_H
#define RENDER_VR_H

#include "types.h"

// Extra interface of the VR renderer (render_vr.c), used by the VR platform.
// The game still talks to the renderer through render.h only. During a frame
// all draw calls are recorded; after system_update() the platform replays
// them into its render targets:
//
// - Stereo: the 3D parts of the frame are drawn once per eye with the eye's
//   pose applied on top of the game camera. The 2D parts (HUD, menus) are
//   drawn into the screen target with a transparent background.
// - Flat: the whole frame is drawn into the screen target, just like the
//   desktop renderer draws it into the window.

// Number of game units per meter. A ship is about 1000 units long.
#define RENDER_VR_UNITS_PER_METER 128.0f

typedef struct {
	float tan_left, tan_right, tan_up, tan_down;
} render_vr_fov_t;

// Replay the 3D draws of the recorded frame. eye_view transforms from the
// game camera's space into eye space, in game units.
void render_vr_draw_eye(uint32_t fbo, vec2i_t size, mat4_t *eye_view, render_vr_fov_t fov);

// Replay the recorded frame into the screen target. With stereo set, only the
// 2D draws are replayed onto a transparent background (the result is
// premultiplied alpha); otherwise the whole frame is drawn opaque.
void render_vr_draw_screen(uint32_t fbo, bool stereo);

#endif
