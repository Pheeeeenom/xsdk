#ifndef XSDK_BAR_SCENE_H
#define XSDK_BAR_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/shader.h"

// A full-height bar moving 8 pixels right a frame across 640x480, so a frame shown late or torn shows on a capture.

enum {
	BAR_FRAME_WIDTH = 640,
	BAR_FRAME_HEIGHT = 480,
};

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	volatile uint32_t *vertices;
	uint32_t argb;
} BarScene;

bool bar_scene_open(BarScene *scene, uint32_t argb);
// Draws the frame's bar and resolves the frame into address.
void bar_scene_draw(const BarScene *scene, uint32_t frame, uint32_t address);
// Whether a resolved frame shows its bar where it belongs, on the clear colour.
bool bar_scene_holds(const BarScene *scene, volatile const uint32_t *pixels, uint32_t frame);
void bar_scene_close(BarScene *scene);

#endif
