#include "controls.h"
#include "fpsoverlay.h"
#include "../utils/logger.h"
#include <switch.h>
#include <math.h>
#include <string.h>
#include <falso_jni/FalsoJNI.h>

#define JNI_PREFIX "Java_com_gameloft_android_ANMP_GloftA8HM_"
#define HID_BRIDGE JNI_PREFIX "GLUtils_controller_NativeBridgeHIDControllers_"
#define MAX_TOUCHES 10

// JNIBridge.NativeOnTouch(int action, float x, float y, int pointerId)
static void (*native_on_touch)(void *env, void *clazz, int action, float x, float y, int pointer_id) = NULL;

// NativeBridgeHIDControllers: what StandardHIDController.java feeds the engine.
static void (*native_controller_connected)(void *env, void *clazz, jstring name) = NULL;
static void (*native_controller_disconnected)(void *env, void *clazz) = NULL;
static void (*native_handle_input)(void *env, void *clazz, int control, double value) = NULL;

// Control ids of NativeHandleInputEvents, as StandardHIDController assigns
// them to Android key codes and axes. Buttons report 1.0 / 0.0.
enum {
    HID_TRIGGER_L2 = 1,   // AXIS_LTRIGGER / AXIS_BRAKE, 0..1
    HID_TRIGGER_R2 = 2,   // AXIS_RTRIGGER / AXIS_GAS, 0..1
    HID_LEFT_X = 3,       // AXIS_X, -1 (left) .. 1; Android TV only
    HID_LEFT_Y = 4,       // AXIS_Y, -1 (up) .. 1; Android TV only
    HID_RIGHT_X = 5,      // AXIS_Z
    HID_RIGHT_Y = 6,      // AXIS_RZ
    HID_DPAD_UP = 7,
    HID_DPAD_DOWN = 8,
    HID_DPAD_LEFT = 9,
    HID_DPAD_RIGHT = 10,
    HID_L1 = 11,
    HID_R1 = 12,
    HID_Y = 13,
    HID_A = 14,
    HID_X = 15,
    HID_B = 16,
    HID_START = 17,
    HID_SELECT = 18,
    HID_BACK = 19,
    HID_THUMB_L = 20,
    HID_THUMB_R = 21,
};

// Buttons go by the letter printed on them, so the prompts the game draws
// match what the player presses.
static const struct { u64 button; int control; } s_button_map[] = {
    { HidNpadButton_A,      HID_A },
    { HidNpadButton_B,      HID_B },
    { HidNpadButton_X,      HID_X },
    { HidNpadButton_Y,      HID_Y },
    { HidNpadButton_L,      HID_L1 },
    { HidNpadButton_R,      HID_R1 },
    { HidNpadButton_ZL,     HID_TRIGGER_L2 },
    { HidNpadButton_ZR,     HID_TRIGGER_R2 },
    { HidNpadButton_Plus,   HID_START },
    { HidNpadButton_Minus,  HID_SELECT },
    { HidNpadButton_StickL, HID_THUMB_L },
    { HidNpadButton_StickR, HID_THUMB_R },
};

#define STICK_DEADZONE 0.15
#define STICK_STEP     0.01   // smallest change worth reporting

// On a phone the engine does not read the left stick's axes at all:
// StandardHIDController only forwards them on Android TV, and otherwise
// leaves the event unhandled so that Android turns the stick into d-pad
// keys. Do the same, with some hysteresis so a held direction does not
// flicker.
#define STICK_DPAD_PRESS   0.50
#define STICK_DPAD_RELEASE 0.35

static const struct { u64 button; int control; } s_dpad_map[4] = {
    { HidNpadButton_Up,    HID_DPAD_UP },
    { HidNpadButton_Down,  HID_DPAD_DOWN },
    { HidNpadButton_Left,  HID_DPAD_LEFT },
    { HidNpadButton_Right, HID_DPAD_RIGHT },
};
static bool s_dpad_sent[4];    // what the engine was last told
static bool s_stick_dir[4];    // up, down, left, right from the left stick

typedef struct {
    int control;
    double sent;   // last value the engine was given
} AxisState;

static AxisState s_axes[2] = {
    { HID_RIGHT_X, 0.0 }, { HID_RIGHT_Y, 0.0 },
};

static PadState s_pad;
static bool s_listener_registered = false;
static bool s_controller_connected = false;

typedef struct {
    bool active;
    u32 finger_id;
    float x, y;
} TouchSlot;

static TouchSlot s_touches[MAX_TOUCHES];

void controls_init(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&s_pad);
    hidInitializeTouchScreen();
}

void controls_resolve(so_module *game) {
    native_on_touch = (void *)so_symbol(game, JNI_PREFIX "PackageUtils_JNIBridge_NativeOnTouch");
    native_controller_connected = (void *)so_symbol(game, HID_BRIDGE "NativeControllerConnected");
    native_controller_disconnected = (void *)so_symbol(game, HID_BRIDGE "NativeControllerDisconnected");
    native_handle_input = (void *)so_symbol(game, HID_BRIDGE "NativeHandleInputEvents");
    if (!native_on_touch) l_warn("[controls] NativeOnTouch not found; touch disabled");
    if (!native_controller_connected || !native_handle_input)
        l_warn("[controls] HID controller bridge not found; gamepad disabled");
}

void controls_set_listener(bool registered) {
    l_info("[controls] game %s its controller listener", registered ? "registered" : "removed");
    __atomic_store_n(&s_listener_registered, registered, __ATOMIC_RELEASE);
}

static void poll_touch(void) {
    if (!native_on_touch) return;

    HidTouchScreenState state = {0};
    if (hidGetTouchScreenStates(&state, 1) == 0) return;

    bool seen[MAX_TOUCHES] = {0};

    for (int i = 0; i < state.count && i < MAX_TOUCHES; i++) {
        const HidTouchState *t = &state.touches[i];
        int slot = -1;
        for (int j = 0; j < MAX_TOUCHES; j++) {
            if (s_touches[j].active && s_touches[j].finger_id == t->finger_id) { slot = j; break; }
        }

        if (slot < 0) {
            for (int j = 0; j < MAX_TOUCHES; j++) {
                if (!s_touches[j].active) { slot = j; break; }
            }
            if (slot < 0) continue;
            s_touches[slot].active = true;
            s_touches[slot].finger_id = t->finger_id;
            s_touches[slot].x = (float)t->x;
            s_touches[slot].y = (float)t->y;
            native_on_touch(&jni, NULL, TOUCH_ACTION_DOWN, (float)t->x, (float)t->y, slot);
        } else if (s_touches[slot].x != (float)t->x || s_touches[slot].y != (float)t->y) {
            s_touches[slot].x = (float)t->x;
            s_touches[slot].y = (float)t->y;
            native_on_touch(&jni, NULL, TOUCH_ACTION_MOVE, (float)t->x, (float)t->y, slot);
        }
        seen[slot] = true;
    }

    for (int j = 0; j < MAX_TOUCHES; j++) {
        if (s_touches[j].active && !seen[j]) {
            s_touches[j].active = false;
            native_on_touch(&jni, NULL, TOUCH_ACTION_UP, s_touches[j].x, s_touches[j].y, j);
        }
    }
}

// Same contract as StandardHIDController: values inside the dead zone are
// not reported, except for one 0 when the stick comes back to rest.
static void report_axis(AxisState *axis, double value) {
    if (fabs(value) <= STICK_DEADZONE) value = 0.0;
    if (value == axis->sent) return;
    if (value != 0.0 && fabs(value - axis->sent) < STICK_STEP) return;
    axis->sent = value;
    native_handle_input(&jni, NULL, axis->control, value);
}

static void poll_gamepad(void) {
    if (!native_controller_connected || !native_handle_input) return;

    // The engine only listens once it has asked Java to start watching for
    // controllers, which is also when Java would announce one.
    bool want = __atomic_load_n(&s_listener_registered, __ATOMIC_ACQUIRE) && padIsConnected(&s_pad);
    if (want != s_controller_connected) {
        s_controller_connected = want;
        if (want) {
            jstring name = jni->NewStringUTF(&jni, "Nintendo Switch Controller");
            native_controller_connected(&jni, NULL, name);
            l_info("[controls] controller connected");
        } else {
            for (size_t i = 0; i < sizeof(s_axes) / sizeof(s_axes[0]); i++) s_axes[i].sent = 0.0;
            memset(s_dpad_sent, 0, sizeof(s_dpad_sent));
            memset(s_stick_dir, 0, sizeof(s_stick_dir));
            if (native_controller_disconnected) native_controller_disconnected(&jni, NULL);
            l_info("[controls] controller disconnected");
        }
    }
    if (!s_controller_connected) return;

    u64 down = padGetButtonsDown(&s_pad);
    u64 up = padGetButtonsUp(&s_pad);
    for (size_t i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
        if (down & s_button_map[i].button) native_handle_input(&jni, NULL, s_button_map[i].control, 1.0);
        if (up & s_button_map[i].button)   native_handle_input(&jni, NULL, s_button_map[i].control, 0.0);
    }

    // Left stick and d-pad buttons drive the same four controls.
    HidAnalogStickState left = padGetStickPos(&s_pad, 0);
    const double push[4] = {
        (double)left.y / JOYSTICK_MAX, -(double)left.y / JOYSTICK_MAX,
        -(double)left.x / JOYSTICK_MAX, (double)left.x / JOYSTICK_MAX,
    };
    u64 held = padGetButtons(&s_pad);
    for (int i = 0; i < 4; i++) {
        s_stick_dir[i] = push[i] > (s_stick_dir[i] ? STICK_DPAD_RELEASE : STICK_DPAD_PRESS);
        bool pressed = s_stick_dir[i] || (held & s_dpad_map[i].button);
        if (pressed == s_dpad_sent[i]) continue;
        s_dpad_sent[i] = pressed;
        native_handle_input(&jni, NULL, s_dpad_map[i].control, pressed ? 1.0 : 0.0);
    }

    // Android's Y axes grow downwards, the Switch's upwards.
    HidAnalogStickState right = padGetStickPos(&s_pad, 1);
    report_axis(&s_axes[0], (double)right.x / JOYSTICK_MAX);
    report_axis(&s_axes[1], -(double)right.y / JOYSTICK_MAX);
}

void controls_poll(void) {
    padUpdate(&s_pad);

    // Both sticks pressed together show or hide the frame rate overlay.
    static bool s_combo_held = false;
    const u64 combo = HidNpadButton_StickL | HidNpadButton_StickR;
    bool held = (padGetButtons(&s_pad) & combo) == combo;
    if (held && !s_combo_held) fpsoverlay_toggle();
    s_combo_held = held;

    poll_gamepad();
    poll_touch();
}
