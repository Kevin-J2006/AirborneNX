#ifndef __REIMPL_FPSOVERLAY_H__
#define __REIMPL_FPSOVERLAY_H__

/*
 * Frame rate overlay: FPS, frame time and a frame time graph drawn over the
 * game's picture. Hidden until the player presses both sticks, or from the
 * start with "fpsoverlay=1" in config.ini.
 */

// Reads config.ini.
void fpsoverlay_init(void);

// Shows or hides the overlay. Any thread.
void fpsoverlay_toggle(void);

// Called for every frame right before it is presented, on the drawing thread
// with the GL lock held: times the frame and draws the overlay if visible.
void fpsoverlay_frame(void);

#endif // __REIMPL_FPSOVERLAY_H__
