#ifndef __REIMPL_CONTROLS_H__
#define __REIMPL_CONTROLS_H__

#include <stdint.h>
#include <stdbool.h>
#include "so_util/so_util.h"

// Touch actions as JNIBridge.NativeOnTouch expects them (v4.0.0l)
#define TOUCH_ACTION_DOWN       0
#define TOUCH_ACTION_MOVE       1
#define TOUCH_ACTION_UP         2
#define TOUCH_ACTION_CANCEL     3

void controls_init(void);
void controls_resolve(so_module *game);
void controls_poll(void);

// The engine asked Java to start (or stop) watching for game controllers.
void controls_set_listener(bool registered);

#endif // __REIMPL_CONTROLS_H__
