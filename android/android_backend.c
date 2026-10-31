/* Android window backend: implements the xwayland_window_* API (platform/xwayland_backend.h) on a NativeActivity.
 *
 *  - window: the activity's ANativeWindow is the one native window; its size is the guest's client rectangle.
 *  - input:  touch -> absolute pointer + left button, hardware keys -> X11 keycodes/keysyms (what the loader's
 *            input code already consumes).
 *  - output: a Vulkan swapchain on VK_KHR_android_surface; frames are blitted straight from the D3D12 back
 *            buffer (no CPU copy).
 *
 * ANativeActivity_onCreate() is the entry point: it wires the callbacks and starts the loader on its own thread.
 * The game to run is named by <external files dir>/launch.txt: line 1 = game directory, line 2 = exe name,
 * remaining lines = extra loader arguments. */
#define _GNU_SOURCE
#include "xwayland_backend.h"
#include "vulkan_context.h"
#include "beer_overlay.h"
#include "beer_gamepad.h"

#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Beer", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Beer", __VA_ARGS__)

extern int beer_loader_main(int argc, char **argv);

/* ---- shared state --------------------------------------------------------------------------------------- */
#define EVENT_RING 512

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    ANativeActivity *activity;
    ANativeWindow *window;
    int window_generation;         /* bumps whenever a new ANativeWindow arrives (the Vulkan surface must follow) */
    int width, height;
    int created;                   /* xwayland_window_create() has run */
    int visible;
    int focused;
    int close_requested;
    XwaylandEvent ring[EVENT_RING];
    unsigned head, tail;
    AInputQueue *queue;
    pthread_t input_thread;
    int input_thread_started;
    ALooper *input_looper;
    float touch_x, touch_y;
} A = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void push_event_locked(const XwaylandEvent *event)
{
    const unsigned next = (A.head + 1) % EVENT_RING;
    if (next == A.tail) return;   /* full: drop */
    A.ring[A.head] = *event;
    A.head = next;
}

static void push_event(const XwaylandEvent *event)
{
    pthread_mutex_lock(&A.lock);
    push_event_locked(event);
    pthread_mutex_unlock(&A.lock);
}

static void push_simple(XwaylandEventType type)
{
    XwaylandEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    pthread_mutex_lock(&A.lock);
    e.width = A.width;
    e.height = A.height;
    push_event_locked(&e);
    pthread_mutex_unlock(&A.lock);
}

/* ---- key mapping ---------------------------------------------------------------------------------------- */
typedef struct { int android; unsigned evdev; unsigned long keysym; } KeyMap;

static const KeyMap key_map[] = {
    { AKEYCODE_A, 30, 'a' }, { AKEYCODE_B, 48, 'b' }, { AKEYCODE_C, 46, 'c' }, { AKEYCODE_D, 32, 'd' },
    { AKEYCODE_E, 18, 'e' }, { AKEYCODE_F, 33, 'f' }, { AKEYCODE_G, 34, 'g' }, { AKEYCODE_H, 35, 'h' },
    { AKEYCODE_I, 23, 'i' }, { AKEYCODE_J, 36, 'j' }, { AKEYCODE_K, 37, 'k' }, { AKEYCODE_L, 38, 'l' },
    { AKEYCODE_M, 50, 'm' }, { AKEYCODE_N, 49, 'n' }, { AKEYCODE_O, 24, 'o' }, { AKEYCODE_P, 25, 'p' },
    { AKEYCODE_Q, 16, 'q' }, { AKEYCODE_R, 19, 'r' }, { AKEYCODE_S, 31, 's' }, { AKEYCODE_T, 20, 't' },
    { AKEYCODE_U, 22, 'u' }, { AKEYCODE_V, 47, 'v' }, { AKEYCODE_W, 17, 'w' }, { AKEYCODE_X, 45, 'x' },
    { AKEYCODE_Y, 21, 'y' }, { AKEYCODE_Z, 44, 'z' },
    { AKEYCODE_1, 2, '1' }, { AKEYCODE_2, 3, '2' }, { AKEYCODE_3, 4, '3' }, { AKEYCODE_4, 5, '4' },
    { AKEYCODE_5, 6, '5' }, { AKEYCODE_6, 7, '6' }, { AKEYCODE_7, 8, '7' }, { AKEYCODE_8, 9, '8' },
    { AKEYCODE_9, 10, '9' }, { AKEYCODE_0, 11, '0' },
    { AKEYCODE_ESCAPE, 1, 0xff1b }, { AKEYCODE_BACK, 1, 0xff1b },
    { AKEYCODE_ENTER, 28, 0xff0d }, { AKEYCODE_NUMPAD_ENTER, 28, 0xff0d },
    { AKEYCODE_SPACE, 57, ' ' }, { AKEYCODE_DEL, 14, 0xff08 }, { AKEYCODE_TAB, 15, 0xff09 },
    { AKEYCODE_SHIFT_LEFT, 42, 0xffe1 }, { AKEYCODE_SHIFT_RIGHT, 54, 0xffe2 },
    { AKEYCODE_CTRL_LEFT, 29, 0xffe3 }, { AKEYCODE_CTRL_RIGHT, 97, 0xffe4 },
    { AKEYCODE_ALT_LEFT, 56, 0xffe9 }, { AKEYCODE_ALT_RIGHT, 100, 0xffea },
    { AKEYCODE_DPAD_UP, 103, 0xff52 }, { AKEYCODE_DPAD_DOWN, 108, 0xff54 },
    { AKEYCODE_DPAD_LEFT, 105, 0xff51 }, { AKEYCODE_DPAD_RIGHT, 106, 0xff53 },
    { AKEYCODE_F1, 59, 0xffbe }, { AKEYCODE_F2, 60, 0xffbf }, { AKEYCODE_F3, 61, 0xffc0 },
    { AKEYCODE_F4, 62, 0xffc1 }, { AKEYCODE_F5, 63, 0xffc2 }, { AKEYCODE_F6, 64, 0xffc3 },
    { AKEYCODE_F7, 65, 0xffc4 }, { AKEYCODE_F8, 66, 0xffc5 }, { AKEYCODE_F9, 67, 0xffc6 },
    { AKEYCODE_F10, 68, 0xffc7 }, { AKEYCODE_F11, 87, 0xffc8 }, { AKEYCODE_F12, 88, 0xffc9 },
    { AKEYCODE_COMMA, 51, ',' }, { AKEYCODE_PERIOD, 52, '.' }, { AKEYCODE_SLASH, 53, '/' },
    { AKEYCODE_SEMICOLON, 39, ';' }, { AKEYCODE_APOSTROPHE, 40, '\'' }, { AKEYCODE_MINUS, 12, '-' },
    { AKEYCODE_EQUALS, 13, '=' }, { AKEYCODE_LEFT_BRACKET, 26, '[' }, { AKEYCODE_RIGHT_BRACKET, 27, ']' },
    { AKEYCODE_BACKSLASH, 43, '\\' }, { AKEYCODE_GRAVE, 41, '`' },
    { AKEYCODE_PAGE_UP, 104, 0xff55 }, { AKEYCODE_PAGE_DOWN, 109, 0xff56 },
    { AKEYCODE_MOVE_HOME, 102, 0xff50 }, { AKEYCODE_MOVE_END, 107, 0xff57 },
    { AKEYCODE_INSERT, 110, 0xff63 }, { AKEYCODE_FORWARD_DEL, 111, 0xffff },
};

static const KeyMap *find_key(int android)
{
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); ++i)
        if (key_map[i].android == android) return &key_map[i];
    return NULL;
}

/* ---- input ---------------------------------------------------------------------------------------------- */
static void emit_pointer(XwaylandEventType type, float x, float y, unsigned button)
{
    XwaylandEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.x = e.root_x = (int)x;
    e.y = e.root_y = (int)y;
    e.button = button;
    push_event(&e);
}

/* Touch as a touchpad: dragging moves the cursor by the finger's movement (games read the mouse as relative motion, so
 * the cursor must not jump to where a finger lands), a short tap is a left click at the cursor, and a two-finger tap is
 * a right click (the "back" of many PC menus). A tap must stay within TAP_SLOP pixels and TAP_NS of its press; anything
 * longer or further is a drag. `pointers` is the number of fingers down. Also fed by the on-screen pad's view for
 * touches that land outside its controls. */
static void touchpad_event(int action, float x, float y, int pointers, int64_t time_ns)
{
    enum { TAP_SLOP = 24 };
    const int64_t TAP_NS = 300000000;
    static float down_x, down_y, last_x, last_y, cursor_x = -1, cursor_y = -1;
    static int64_t down_time;
    static int moved, max_pointers;
    if (cursor_x < 0) { cursor_x = A.width / 2.0f; cursor_y = A.height / 2.0f; }
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        down_x = last_x = x; down_y = last_y = y;
        down_time = time_ns;
        moved = 0;
        max_pointers = 1;
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (pointers > max_pointers) max_pointers = pointers;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        if (fabsf(x - down_x) > TAP_SLOP || fabsf(y - down_y) > TAP_SLOP) moved = 1;
        if (max_pointers == 1) {
            cursor_x = fminf(fmaxf(cursor_x + (x - last_x), 0), (float)(A.width > 0 ? A.width - 1 : 0));
            cursor_y = fminf(fmaxf(cursor_y + (y - last_y), 0), (float)(A.height > 0 ? A.height - 1 : 0));
            emit_pointer(XWAYLAND_EVENT_POINTER_MOTION, cursor_x, cursor_y, 0);
        }
        last_x = x; last_y = y;
        break;
    case AMOTION_EVENT_ACTION_UP:
        if (!moved && time_ns - down_time <= TAP_NS * (max_pointers > 1 ? 2 : 1)) {
            const unsigned button = max_pointers > 1 ? 3 : 1;   /* X11: 1 left, 3 right */
            emit_pointer(XWAYLAND_EVENT_BUTTON_DOWN, cursor_x, cursor_y, button);
            usleep(50000);   /* hold it across a few frames: games that poll the mouse once per frame see it */
            emit_pointer(XWAYLAND_EVENT_BUTTON_UP, cursor_x, cursor_y, button);
        }
        break;
    default:
        break;
    }
}

/* A physical gamepad (Bluetooth/USB): Android key codes for its buttons, joystick axes for sticks, triggers and hat. */
static BeerGamepadState g_physical_pad;

static uint16_t gamepad_button(int keycode)
{
    switch (keycode) {
    case AKEYCODE_BUTTON_A: return BEER_PAD_A;
    case AKEYCODE_BUTTON_B: return BEER_PAD_B;
    case AKEYCODE_BUTTON_X: return BEER_PAD_X;
    case AKEYCODE_BUTTON_Y: return BEER_PAD_Y;
    case AKEYCODE_BUTTON_L1: return BEER_PAD_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1: return BEER_PAD_RIGHT_SHOULDER;
    case AKEYCODE_BUTTON_THUMBL: return BEER_PAD_LEFT_THUMB;
    case AKEYCODE_BUTTON_THUMBR: return BEER_PAD_RIGHT_THUMB;
    case AKEYCODE_BUTTON_START: return BEER_PAD_START;
    case AKEYCODE_BUTTON_SELECT: return BEER_PAD_BACK;
    case AKEYCODE_DPAD_UP: return BEER_PAD_DPAD_UP;
    case AKEYCODE_DPAD_DOWN: return BEER_PAD_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return BEER_PAD_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return BEER_PAD_DPAD_RIGHT;
    default: return 0;
    }
}

static int16_t axis16(float v, int invert)
{
    if (invert) v = -v;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

static int gamepad_event(AInputEvent *event)
{
    const int type = AInputEvent_getType(event);
    BeerGamepadState *p = &g_physical_pad;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int keycode = AKeyEvent_getKeyCode(event);
        const int down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
        if (keycode == AKEYCODE_BUTTON_L2) p->left_trigger = down ? 255 : 0;
        else if (keycode == AKEYCODE_BUTTON_R2) p->right_trigger = down ? 255 : 0;
        else {
            const uint16_t bit = gamepad_button(keycode);
            if (!bit) return 0;
            p->buttons = down ? (uint16_t)(p->buttons | bit) : (uint16_t)(p->buttons & ~bit);
        }
    } else {
        p->lx = axis16(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0), 0);
        p->ly = axis16(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0), 1);
        p->rx = axis16(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0), 0);
        p->ry = axis16(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0), 1);
        const float lt = fmaxf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                               AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
        const float rt = fmaxf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                               AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));
        p->left_trigger = (uint8_t)(fminf(fmaxf(lt, 0), 1) * 255);
        p->right_trigger = (uint8_t)(fminf(fmaxf(rt, 0), 1) * 255);
        const float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        const float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        p->buttons &= (uint16_t)~(BEER_PAD_DPAD_UP | BEER_PAD_DPAD_DOWN | BEER_PAD_DPAD_LEFT | BEER_PAD_DPAD_RIGHT);
        if (hx < -0.5f) p->buttons |= BEER_PAD_DPAD_LEFT;
        if (hx > 0.5f) p->buttons |= BEER_PAD_DPAD_RIGHT;
        if (hy < -0.5f) p->buttons |= BEER_PAD_DPAD_UP;
        if (hy > 0.5f) p->buttons |= BEER_PAD_DPAD_DOWN;
    }
    beer_gamepad_update(BEER_PAD_SOURCE_PHYSICAL, 1, p);
    return 1;
}

static int handle_input_event(AInputEvent *event)
{
    const int type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);
    if ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
        ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK && type == AINPUT_EVENT_TYPE_MOTION)) {
        if (gamepad_event(event)) return 1;
    }
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const KeyMap *map = find_key(AKeyEvent_getKeyCode(event));
        if (!map) return 0;
        const int action = AKeyEvent_getAction(event);
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 0;
        XwaylandEvent e;
        memset(&e, 0, sizeof(e));
        e.type = action == AKEY_EVENT_ACTION_DOWN ? XWAYLAND_EVENT_KEY_DOWN : XWAYLAND_EVENT_KEY_UP;
        e.keycode = map->evdev + 8;   /* X11 keycode = evdev code + 8 */
        e.keysym = map->keysym;
        push_event(&e);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        const int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if (action != AMOTION_EVENT_ACTION_DOWN && action != AMOTION_EVENT_ACTION_POINTER_DOWN &&
            action != AMOTION_EVENT_ACTION_MOVE && action != AMOTION_EVENT_ACTION_UP &&
            action != AMOTION_EVENT_ACTION_POINTER_UP && action != AMOTION_EVENT_ACTION_CANCEL)
            return 0;
        touchpad_event(action, AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0),
                       (int)AMotionEvent_getPointerCount(event), AMotionEvent_getEventTime(event));
        return 1;
    }
    return 0;
}

/* The on-screen pad (android/apk/java/.../PadView.java) reports the whole controller state, and passes touches that miss
 * its controls on to the touchpad. */
JNIEXPORT void JNICALL Java_com_beer_loader_PadView_nativePad(JNIEnv *env, jclass cls, jboolean connected, jint buttons,
                                                             jint lt, jint rt, jint lx, jint ly, jint rx, jint ry)
{
    (void)env; (void)cls;
    const BeerGamepadState state = { (uint16_t)buttons, (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly,
                                     (int16_t)rx, (int16_t)ry };
    beer_gamepad_update(BEER_PAD_SOURCE_ONSCREEN, connected, &state);
}

JNIEXPORT void JNICALL Java_com_beer_loader_PadView_nativeTouch(JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y,
                                                               jint pointers, jlong time_ns)
{
    (void)env; (void)cls;
    touchpad_event(action, x, y, pointers, time_ns);
}

static int input_callback(int fd, int events, void *data)
{
    (void)fd; (void)events; (void)data;
    AInputQueue *queue = A.queue;
    AInputEvent *event = NULL;
    while (queue && AInputQueue_getEvent(queue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(queue, event)) continue;
        AInputQueue_finishEvent(queue, event, handle_input_event(event));
    }
    return 1;
}

static void *input_thread_main(void *unused)
{
    (void)unused;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    pthread_mutex_lock(&A.lock);
    A.input_looper = looper;
    if (A.queue) AInputQueue_attachLooper(A.queue, looper, 1, input_callback, NULL);
    pthread_cond_broadcast(&A.cond);
    pthread_mutex_unlock(&A.lock);
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}

/* ---- activity callbacks --------------------------------------------------------------------------------- */
static void on_window_created(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity;
    ANativeWindow_acquire(window);
    pthread_mutex_lock(&A.lock);
    A.window = window;
    A.width = ANativeWindow_getWidth(window);
    A.height = ANativeWindow_getHeight(window);
    ++A.window_generation;
    pthread_cond_broadcast(&A.cond);
    pthread_mutex_unlock(&A.lock);
    LOGI("window created %dx%d", A.width, A.height);
}

static void on_window_destroyed(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    if (A.window == window) A.window = NULL;
    ++A.window_generation;
    pthread_mutex_unlock(&A.lock);
    ANativeWindow_release(window);
}

static void on_window_resized(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    A.width = ANativeWindow_getWidth(window);
    A.height = ANativeWindow_getHeight(window);
    pthread_mutex_unlock(&A.lock);
    push_simple(XWAYLAND_EVENT_CONFIGURE);
}

static void on_window_redraw(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity; (void)window;
    push_simple(XWAYLAND_EVENT_EXPOSE);
}

static void on_focus_changed(ANativeActivity *activity, int focused)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    A.focused = focused;
    pthread_mutex_unlock(&A.lock);
    push_simple(focused ? XWAYLAND_EVENT_FOCUS_IN : XWAYLAND_EVENT_FOCUS_OUT);
}

static void on_input_queue_created(ANativeActivity *activity, AInputQueue *queue)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    A.queue = queue;
    if (A.input_looper) {
        AInputQueue_attachLooper(queue, A.input_looper, 1, input_callback, NULL);
    } else if (!A.input_thread_started) {
        A.input_thread_started = 1;
        pthread_create(&A.input_thread, NULL, input_thread_main, NULL);
    }
    pthread_mutex_unlock(&A.lock);
}

static void on_input_queue_destroyed(ANativeActivity *activity, AInputQueue *queue)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    if (A.queue == queue) {
        AInputQueue_detachLooper(queue);
        A.queue = NULL;
    }
    pthread_mutex_unlock(&A.lock);
}

static void on_destroy(ANativeActivity *activity)
{
    (void)activity;
    pthread_mutex_lock(&A.lock);
    A.close_requested = 1;
    pthread_mutex_unlock(&A.lock);
    push_simple(XWAYLAND_EVENT_CLOSE);
    LOGI("activity destroyed");
    _exit(0);   /* the guest owns the process; there is nothing to unwind */
}

/* Games live in shared storage, which needs "all files access". Open the system page for it when it isn't granted. */
static void request_all_files_access(ANativeActivity *activity)
{
    JNIEnv *env = activity->env;
    jclass environment = (*env)->FindClass(env, "android/os/Environment");
    if (!environment) { (*env)->ExceptionClear(env); return; }
    jmethodID is_manager = (*env)->GetStaticMethodID(env, environment, "isExternalStorageManager", "()Z");
    if (!is_manager) { (*env)->ExceptionClear(env); return; }
    if ((*env)->CallStaticBooleanMethod(env, environment, is_manager)) return;

    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID package_name = (*env)->GetMethodID(env, activity_class, "getPackageName", "()Ljava/lang/String;");
    jstring package = (*env)->CallObjectMethod(env, activity->clazz, package_name);
    const char *package_chars = package ? (*env)->GetStringUTFChars(env, package, NULL) : NULL;
    char uri_text[256];
    snprintf(uri_text, sizeof(uri_text), "package:%s", package_chars ? package_chars : "com.beer.loader");
    if (package_chars) (*env)->ReleaseStringUTFChars(env, package, package_chars);
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, (*env)->NewStringUTF(env, uri_text));
    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    jmethodID init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jobject intent = (*env)->NewObject(env, intent_class, init,
        (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    jmethodID start = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V");
    (*env)->CallVoidMethod(env, activity->clazz, start, intent);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    LOGI("asked for all-files access");
}

/* The start screen (android/apk/java/.../Launcher.java) opens this activity with the path of a session file in the
 * launch.txt format; started any other way (adb, tests), launch.txt is read as before. */
static char g_session_path[1024];

static void read_session_extra(ANativeActivity *activity)
{
    JNIEnv *env = activity->env;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_intent = (*env)->GetMethodID(env, activity_class, "getIntent", "()Landroid/content/Intent;");
    jobject intent = get_intent ? (*env)->CallObjectMethod(env, activity->clazz, get_intent) : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!intent) return;
    jclass intent_class = (*env)->GetObjectClass(env, intent);
    jmethodID get_extra = (*env)->GetMethodID(env, intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring value = get_extra ? (jstring)(*env)->CallObjectMethod(env, intent, get_extra,
                                                                   (*env)->NewStringUTF(env, "beer.session")) : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!value) return;
    const char *text = (*env)->GetStringUTFChars(env, value, NULL);
    if (text) {
        snprintf(g_session_path, sizeof(g_session_path), "%s", text);
        (*env)->ReleaseStringUTFChars(env, value, text);
        LOGI("session from the start screen: %s", g_session_path);
    }
}

/* The loader logs to stderr; send it (and stdout) to logcat under the tag "Beer" so `adb logcat -s Beer` shows it. */
static void *log_pump(void *fd_ptr)
{
    const int fd = (int)(intptr_t)fd_ptr;
    char buffer[4096];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buffer + used, sizeof(buffer) - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        char *line = buffer, *newline;
        while ((newline = memchr(line, '\n', buffer + used - line))) {
            *newline = 0;
            __android_log_write(ANDROID_LOG_INFO, "BeerLog", line);
            line = newline + 1;
        }
        used = (size_t)(buffer + used - line);
        memmove(buffer, line, used);
        if (used >= sizeof(buffer) - 1) {   /* a line longer than the buffer: flush it */
            buffer[used] = 0;
            __android_log_write(ANDROID_LOG_INFO, "BeerLog", buffer);
            used = 0;
        }
    }
    return NULL;
}

static void redirect_output_to_logcat(void)
{
    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t thread;
    pthread_create(&thread, NULL, log_pump, (void *)(intptr_t)fds[0]);
    pthread_detach(thread);
}

/* ---- loader thread -------------------------------------------------------------------------------------- */
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *, void *, size_t);

static char *read_line(FILE *f, char *buf, size_t size)
{
    if (!fgets(buf, (int)size, f)) return NULL;
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}

static void *loader_thread(void *unused)
{
    (void)unused;
    /* Private, always-writable storage for the prefix, caches and temp files; launch.txt is also looked up in the
     * app's external folder and in shared storage. */
    const char *base = A.activity->internalDataPath ? A.activity->internalDataPath : A.activity->externalDataPath;
    const char *external = A.activity->externalDataPath ? A.activity->externalDataPath : base;
    char path[1024], dir[1024] = "", exe[512] = "", line[1024];
    /* launch.txt: the app's own folder first, then shared storage (writable from Termux or a file manager). The
     * permission page may still be open, so keep looking for a while. */
    FILE *f = NULL;
    if (g_session_path[0]) {
        snprintf(path, sizeof(path), "%s", g_session_path);
        f = fopen(path, "r");
    }
    for (int attempt = 0; attempt < 300 && !f; ++attempt) {
        snprintf(path, sizeof(path), "%s/launch.txt", external);
        f = fopen(path, "r");
        if (!f) {
            snprintf(path, sizeof(path), "/storage/emulated/0/Download/Beer/launch.txt");
            f = fopen(path, "r");
        }
        if (!f) sleep(1);
    }
    char *args[16];
    int argc = 0;
    args[argc++] = "beer";
    char extra[8][512];
    int extras = 0;
    if (f) {
        read_line(f, dir, sizeof(dir));
        read_line(f, exe, sizeof(exe));
        while (extras < 8 && read_line(f, line, sizeof(line))) {
            if (!line[0]) continue;
            if (!strncmp(line, "env:", 4)) {   /* env:NAME=value sets a loader debug option, e.g. env:BEER_BD12_FPS=1 */
                char *eq = strchr(line + 4, '=');
                if (eq) { *eq = 0; setenv(line + 4, eq + 1, 1); }
                continue;
            }
            snprintf(extra[extras], sizeof(extra[extras]), "%s", line);
            args[argc++] = extra[extras++];
        }
        fclose(f);
    }
    if (!dir[0] || !exe[0]) {
        LOGE("no game configured: write the game directory (line 1) and exe name (line 2) to %s", path);
        return NULL;
    }
    if (chdir(dir) != 0) { LOGE("cannot enter %s: %s", dir, strerror(errno)); return NULL; }
    args[argc++] = exe;
    args[argc] = NULL;

    /* Environment the Android build needs (see ANDROID_PORT.md). */
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s/tmp", base);
    mkdir(tmp, 0700);
    setenv("TMPDIR", tmp, 0);
    setenv("HOME", base, 1);   /* the app's default HOME is not writable; the prefix, caches and saves live here */
    char prefix[1100];
    snprintf(prefix, sizeof(prefix), "%s/prefix", base);
    setenv("BEER_PREFIX", prefix, 1);
    /* The shader translators ship next to libbeer.so in the app's native library directory. */
    Dl_info here;
    if (dladdr((void *)ANativeActivity_onCreate, &here) && here.dli_fname) {
        char libdir[1024], value[1100];
        snprintf(libdir, sizeof(libdir), "%s", here.dli_fname);
        char *slash = strrchr(libdir, '/');
        if (slash) {
            *slash = 0;
            snprintf(value, sizeof(value), "%s/libvkd3d-shader.so", libdir);
            setenv("BEER_VKD3D_SHADER_LIBRARY", value, 0);
            snprintf(value, sizeof(value), "%s/libdxil-spirv-c-shared.so", libdir);
            setenv("BEER_DXIL_SPIRV_LIBRARY", value, 0);
            /* The video decoders (media/media_foundation.c) are executables packaged as lib*.so so that Android
             * installs them executable in the same directory. */
            snprintf(value, sizeof(value), "%s/libffmpeg.so", libdir);
            setenv("BEER_FFMPEG", value, 0);
            snprintf(value, sizeof(value), "%s/libffprobe.so", libdir);
            setenv("BEER_FFPROBE", value, 0);
        }
    }
    setenv("BEER_VULKAN_LIBRARY", "libvulkan.so", 0);
    LOGI("starting %s in %s (home %s)", exe, dir, base);
    int rc = beer_loader_main(argc, args);
    LOGI("loader exited with %d", rc);
    _exit(rc);
    return NULL;
}

__attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state, size_t saved_state_size)
{
    (void)saved_state; (void)saved_state_size;
    A.activity = activity;
    redirect_output_to_logcat();
    read_session_extra(activity);
    request_all_files_access(activity);
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onNativeWindowResized = on_window_resized;
    activity->callbacks->onNativeWindowRedrawNeeded = on_window_redraw;
    activity->callbacks->onWindowFocusChanged = on_focus_changed;
    activity->callbacks->onInputQueueCreated = on_input_queue_created;
    activity->callbacks->onInputQueueDestroyed = on_input_queue_destroyed;
    activity->callbacks->onDestroy = on_destroy;
    pthread_t thread;
    pthread_create(&thread, NULL, loader_thread, NULL);
    pthread_detach(thread);
}

/* ---- xwayland_window_* API ------------------------------------------------------------------------------ */
int xwayland_set_presenter(XwaylandPresenter presenter) { (void)presenter; return 1; }

const char *xwayland_presenter_name(XwaylandPresenter presenter)
{
    (void)presenter;
    return "android";
}

int xwayland_window_create(int x, int y, int width, int height, const char *title, int initially_visible)
{
    (void)x; (void)y; (void)width; (void)height; (void)title;
    pthread_mutex_lock(&A.lock);
    while (!A.window) pthread_cond_wait(&A.cond, &A.lock);   /* the activity may still be starting */
    A.created = 1;
    A.visible = initially_visible;
    pthread_mutex_unlock(&A.lock);
    fprintf(stderr, "[ANDROID] window %dx%d\n", A.width, A.height);
    return 1;
}

int xwayland_window_show(int visible)
{
    pthread_mutex_lock(&A.lock);
    A.visible = visible;
    pthread_mutex_unlock(&A.lock);
    push_simple(visible ? XWAYLAND_EVENT_SHOW : XWAYLAND_EVENT_HIDE);
    return 1;
}

int xwayland_window_request_focus(void) { return 1; }
int xwayland_window_focus_lost_by_user(void) { return 0; }
int xwayland_window_set_style(int decorated, int resizable) { (void)decorated; (void)resizable; return 1; }
int xwayland_window_set_fullscreen(int fullscreen) { (void)fullscreen; return 1; }
int xwayland_window_move_resize(int x, int y, int width, int height, int move, int resize)
{ (void)x; (void)y; (void)width; (void)height; (void)move; (void)resize; return 1; }
int xwayland_window_set_client_rect(int cx, int cy, int cw, int ch) { (void)cx; (void)cy; (void)cw; (void)ch; return 1; }
int xwayland_window_set_title(const char *title) { (void)title; return 1; }
int xwayland_window_set_app_id(const char *app_id) { (void)app_id; return 1; }
int xwayland_window_set_icon_argb(const uint32_t *pixels, int width, int height)
{ (void)pixels; (void)width; (void)height; return 1; }
int xwayland_window_set_cursor_visible(int visible) { (void)visible; return 1; }
int xwayland_window_set_cursor_position(int x, int y) { (void)x; (void)y; return 1; }
int xwayland_window_set_pointer_confine(int enable) { (void)enable; return 1; }
int xwayland_raw_motion_active(void) { return 0; }

int xwayland_window_get_state(XwaylandWindowState *state)
{
    if (!state) return 0;
    pthread_mutex_lock(&A.lock);
    state->x = state->y = 0;
    state->width = state->client_width = A.width;
    state->height = state->client_height = A.height;
    state->visible = A.visible;
    pthread_mutex_unlock(&A.lock);
    return 1;
}

void xwayland_get_screen_size(int *out_width, int *out_height)
{
    pthread_mutex_lock(&A.lock);
    if (out_width) *out_width = A.width > 0 ? A.width : 1920;
    if (out_height) *out_height = A.height > 0 ? A.height : 1080;
    pthread_mutex_unlock(&A.lock);
}

int xwayland_get_screen_refresh(void) { return 60; }

int xwayland_window_poll_event(XwaylandEvent *event)
{
    int got = 0;
    pthread_mutex_lock(&A.lock);
    if (A.tail != A.head) {
        *event = A.ring[A.tail];
        A.tail = (A.tail + 1) % EVENT_RING;
        got = 1;
    }
    pthread_mutex_unlock(&A.lock);
    return got;
}

int xwayland_window_pump_events(void)
{
    int close_requested = 0;
    XwaylandEvent event;
    while (xwayland_window_poll_event(&event)) close_requested |= event.type == XWAYLAND_EVENT_CLOSE;
    return close_requested;
}

int xwayland_window_cpu_presenter(void) { return 0; }
int xwayland_window_exists(void) { return A.created; }

/* ---- Vulkan presenter ----------------------------------------------------------------------------------- */
#define FRAMES_IN_FLIGHT 1
#define MAX_SWAP_IMAGES 8

static struct {
    int ready, failed;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    int generation;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    VkFormat format;
    uint32_t image_count;
    VkImage images[MAX_SWAP_IMAGES];
    VkSemaphore render_done[MAX_SWAP_IMAGES];
    VkSemaphore acquire_sem;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    /* RGBA8 UNORM intermediate: raw copy target for sRGB sources and the CPU upload target. */
    VkImage mid;
    VkDeviceMemory mid_memory;
    uint32_t mid_w, mid_h;
    int mid_initialized;
    VkBuffer stage;
    VkDeviceMemory stage_memory;
    uint8_t *stage_map;
    VkDeviceSize stage_size;
    VkBuffer overlay;                 /* host-visible: the overlay boxes, one region per box */
    VkDeviceMemory overlay_memory;
    uint8_t *overlay_map;
    uint64_t overlay_serial[2];
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
#define F(name) PFN_vk##name name;
    F(CreateAndroidSurfaceKHR) F(DestroySurfaceKHR) F(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    F(GetPhysicalDeviceSurfaceFormatsKHR) F(GetPhysicalDeviceSurfaceSupportKHR)
    F(GetPhysicalDeviceMemoryProperties)
    F(CreateSwapchainKHR) F(DestroySwapchainKHR) F(GetSwapchainImagesKHR) F(AcquireNextImageKHR) F(QueuePresentKHR)
    F(CreateSemaphore) F(DestroySemaphore) F(CreateCommandPool) F(AllocateCommandBuffers)
    F(ResetCommandBuffer) F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdPipelineBarrier)
    F(CmdBlitImage) F(CmdCopyImage) F(CmdCopyBufferToImage) F(QueueSubmit)
    F(CreateFence) F(WaitForFences) F(ResetFences) F(DeviceWaitIdle)
    F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) F(BindImageMemory)
    F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(BindBufferMemory)
    F(AllocateMemory) F(FreeMemory) F(MapMemory)
#undef F
} P;

#define LOAD_I(name) do { P.name = (PFN_vk##name)P.gipa(P.instance, "vk" #name); if (!P.name) { LOGE("missing vk" #name); return 0; } } while (0)
#define LOAD_D(name) do { P.name = (PFN_vk##name)P.gdpa(P.device, "vk" #name); if (!P.name) { LOGE("missing vk" #name); return 0; } } while (0)

static int presenter_init(void)
{
    if (P.ready) return 1;
    if (P.failed) return 0;
    P.failed = 1;
    if (!beer_vulkan_context_initialize()) return 0;
    P.instance = beer_vulkan_instance();
    P.physical = beer_vulkan_physical_device();
    P.device = beer_vulkan_device();
    P.queue = beer_vulkan_queue();
    P.family = beer_vulkan_queue_family();
    P.gipa = beer_vulkan_get_instance_proc_addr();
    P.gdpa = beer_vulkan_get_device_proc_addr();
    if (!P.instance || !P.device || !P.gipa || !P.gdpa) return 0;
    LOAD_I(CreateAndroidSurfaceKHR); LOAD_I(DestroySurfaceKHR); LOAD_I(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_I(GetPhysicalDeviceSurfaceFormatsKHR); LOAD_I(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_I(GetPhysicalDeviceMemoryProperties);
    LOAD_D(CreateSwapchainKHR); LOAD_D(DestroySwapchainKHR); LOAD_D(GetSwapchainImagesKHR);
    LOAD_D(AcquireNextImageKHR); LOAD_D(QueuePresentKHR);
    LOAD_D(CreateSemaphore); LOAD_D(DestroySemaphore); LOAD_D(CreateCommandPool); LOAD_D(AllocateCommandBuffers);
    LOAD_D(ResetCommandBuffer); LOAD_D(BeginCommandBuffer); LOAD_D(EndCommandBuffer); LOAD_D(CmdPipelineBarrier);
    LOAD_D(CmdBlitImage); LOAD_D(CmdCopyImage); LOAD_D(CmdCopyBufferToImage); LOAD_D(QueueSubmit);
    LOAD_D(CreateFence); LOAD_D(WaitForFences); LOAD_D(ResetFences); LOAD_D(DeviceWaitIdle);
    LOAD_D(CreateImage); LOAD_D(DestroyImage); LOAD_D(GetImageMemoryRequirements); LOAD_D(BindImageMemory);
    LOAD_D(CreateBuffer); LOAD_D(DestroyBuffer); LOAD_D(GetBufferMemoryRequirements); LOAD_D(BindBufferMemory);
    LOAD_D(AllocateMemory); LOAD_D(FreeMemory); LOAD_D(MapMemory);

    VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = P.family };
    if (P.CreateCommandPool(P.device, &pool, NULL, &P.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = P.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (P.AllocateCommandBuffers(P.device, &cbi, &P.cb) != VK_SUCCESS) return 0;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (P.CreateSemaphore(P.device, &sem, NULL, &P.acquire_sem) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (P.CreateFence(P.device, &fence, NULL, &P.fence) != VK_SUCCESS) return 0;
    P.failed = 0;
    P.ready = 1;
    return 1;
}

static void swapchain_destroy(void)
{
    if (!P.device) return;
    P.DeviceWaitIdle(P.device);
    for (uint32_t i = 0; i < P.image_count; ++i)
        if (P.render_done[i]) { P.DestroySemaphore(P.device, P.render_done[i], NULL); P.render_done[i] = VK_NULL_HANDLE; }
    if (P.swapchain) P.DestroySwapchainKHR(P.device, P.swapchain, NULL);
    if (P.surface) P.DestroySurfaceKHR(P.instance, P.surface, NULL);
    P.swapchain = VK_NULL_HANDLE;
    P.surface = VK_NULL_HANDLE;
    P.image_count = 0;
}

/* (Re)creates the surface and swapchain for the current ANativeWindow. */
static int swapchain_ensure(void)
{
    pthread_mutex_lock(&A.lock);
    ANativeWindow *window = A.window;
    const int generation = A.window_generation;
    pthread_mutex_unlock(&A.lock);
    if (!window) return 0;   /* backgrounded: nothing to present to */
    if (P.swapchain && P.generation == generation) return 1;

    swapchain_destroy();
    VkAndroidSurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window };
    if (P.CreateAndroidSurfaceKHR(P.instance, &info, NULL, &P.surface) != VK_SUCCESS) { LOGE("vkCreateAndroidSurfaceKHR failed"); return 0; }
    VkBool32 supported = VK_FALSE;
    P.GetPhysicalDeviceSurfaceSupportKHR(P.physical, P.family, P.surface, &supported);
    if (!supported) { LOGE("queue family cannot present to the Android surface"); return 0; }

    VkSurfaceCapabilitiesKHR caps;
    if (P.GetPhysicalDeviceSurfaceCapabilitiesKHR(P.physical, P.surface, &caps) != VK_SUCCESS) return 0;
    uint32_t format_count = 0;
    P.GetPhysicalDeviceSurfaceFormatsKHR(P.physical, P.surface, &format_count, NULL);
    VkSurfaceFormatKHR formats[32];
    if (format_count > 32) format_count = 32;
    P.GetPhysicalDeviceSurfaceFormatsKHR(P.physical, P.surface, &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < format_count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = formats[i]; break; }
    P.format = chosen.format;
    P.extent = caps.currentExtent;
    if (P.extent.width == 0xFFFFFFFFu) {
        P.extent.width = (uint32_t)ANativeWindow_getWidth(window);
        P.extent.height = (uint32_t)ANativeWindow_getHeight(window);
    }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;
    VkSwapchainCreateInfoKHR sc = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = P.surface, .minImageCount = count,
        .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace, .imageExtent = P.extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        /* Identity: if the display is rotated the compositor does it, so the guest image is never pre-rotated here. */
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE
    };
    if (!(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) sc.preTransform = caps.currentTransform;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) sc.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (P.CreateSwapchainKHR(P.device, &sc, NULL, &P.swapchain) != VK_SUCCESS) { LOGE("vkCreateSwapchainKHR failed"); return 0; }
    P.image_count = MAX_SWAP_IMAGES;
    P.GetSwapchainImagesKHR(P.device, P.swapchain, &P.image_count, P.images);
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < P.image_count; ++i)
        if (P.CreateSemaphore(P.device, &sem, NULL, &P.render_done[i]) != VK_SUCCESS) return 0;
    P.generation = generation;
    LOGI("swapchain %ux%u format=%d images=%u", P.extent.width, P.extent.height, (int)P.format, P.image_count);
    return 1;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    P.GetPhysicalDeviceMemoryProperties(P.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int mid_ensure(uint32_t w, uint32_t h)
{
    if (P.mid && P.mid_w == w && P.mid_h == h) return 1;
    if (P.mid) {
        P.DeviceWaitIdle(P.device);
        P.DestroyImage(P.device, P.mid, NULL);
        P.FreeMemory(P.device, P.mid_memory, NULL);
        P.mid = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (P.CreateImage(P.device, &ci, NULL, &P.mid) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    P.GetImageMemoryRequirements(P.device, P.mid, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
    if (P.AllocateMemory(P.device, &ai, NULL, &P.mid_memory) != VK_SUCCESS) return 0;
    P.BindImageMemory(P.device, P.mid, P.mid_memory, 0);
    P.mid_w = w; P.mid_h = h; P.mid_initialized = 0;
    return 1;
}

static int stage_ensure(VkDeviceSize size)
{
    if (P.stage && P.stage_size >= size) return 1;
    if (P.stage) {
        P.DeviceWaitIdle(P.device);
        P.DestroyBuffer(P.device, P.stage, NULL);
        P.FreeMemory(P.device, P.stage_memory, NULL);
        P.stage = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (P.CreateBuffer(P.device, &bi, NULL, &P.stage) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    P.GetBufferMemoryRequirements(P.device, P.stage, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
    if (P.AllocateMemory(P.device, &ai, NULL, &P.stage_memory) != VK_SUCCESS) return 0;
    P.BindBufferMemory(P.device, P.stage, P.stage_memory, 0);
    void *map = NULL;
    if (P.MapMemory(P.device, P.stage_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return 0;
    P.stage_map = map;
    P.stage_size = size;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    P.CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Performance overlay (platform/beer_overlay.c): the CPU-rendered boxes are copied onto the swapchain image after the
 * frame was blitted into it (image in TRANSFER_DST). The previous present's fence was waited for, so the buffer is
 * free to rewrite; a box is re-uploaded only when its pixels changed. */
#define OVERLAY_REGION_BYTES (4u * 1024u * 1024u)
static void record_overlay(VkCommandBuffer cb, VkImage image)
{
    BeerOverlayRect rects[2];
    const int count = beer_overlay_frame((int)P.extent.width, (int)P.extent.height, rects);
    if (count <= 0) return;
    if (!P.overlay) {
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2u * OVERLAY_REGION_BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        if (P.CreateBuffer(P.device, &bi, NULL, &P.overlay) != VK_SUCCESS) { P.overlay = VK_NULL_HANDLE; return; }
        VkMemoryRequirements req;
        P.GetBufferMemoryRequirements(P.device, P.overlay, &req);
        const int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = (uint32_t)type };
        void *map = NULL;
        if (type < 0 || P.AllocateMemory(P.device, &ai, NULL, &P.overlay_memory) != VK_SUCCESS ||
            P.BindBufferMemory(P.device, P.overlay, P.overlay_memory, 0) != VK_SUCCESS ||
            P.MapMemory(P.device, P.overlay_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
            P.DestroyBuffer(P.device, P.overlay, NULL);
            P.overlay = VK_NULL_HANDLE;
            return;
        }
        P.overlay_map = map;
        P.overlay_serial[0] = P.overlay_serial[1] = 0;
    }
    const int swap_rb = P.format == VK_FORMAT_R8G8B8A8_UNORM;   /* the boxes are B G R A */
    for (int i = 0; i < count; ++i) {
        const BeerOverlayRect *r = &rects[i];
        if (r->w <= 0 || r->h <= 0 || r->x < 0 || r->y < 0 || (uint32_t)(r->x + r->w) > P.extent.width ||
            (uint32_t)(r->y + r->h) > P.extent.height)
            continue;
        const size_t bytes = (size_t)r->w * (size_t)r->h * 4u;
        if (bytes > OVERLAY_REGION_BYTES) continue;
        const int slot = (int)(r->serial & 1u);   /* the box index is the serial's low bit */
        uint8_t *region = P.overlay_map + (size_t)slot * OVERLAY_REGION_BYTES;
        if (P.overlay_serial[slot] != r->serial) {
            if (swap_rb) {
                for (size_t k = 0; k < bytes; k += 4) {
                    region[k] = r->bgra[k + 2]; region[k + 1] = r->bgra[k + 1];
                    region[k + 2] = r->bgra[k]; region[k + 3] = r->bgra[k + 3];
                }
            } else {
                memcpy(region, r->bgra, bytes);
            }
            P.overlay_serial[slot] = r->serial;
        }
        VkBufferImageCopy copy = { .bufferOffset = (VkDeviceSize)slot * OVERLAY_REGION_BYTES, .bufferRowLength = (uint32_t)r->w,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { r->x, r->y, 0 },
            .imageExtent = { (uint32_t)r->w, (uint32_t)r->h, 1 } };
        P.CmdCopyBufferToImage(cb, P.overlay, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
}

/* kind: 0 = blit `source` as-is, 1 = raw-copy `source` into the RGBA8 intermediate first, 2 = upload the staging
 * buffer into the intermediate. The result is blitted (scaled) to the swapchain image and presented. */
static int present_common(VkImage source, VkImageLayout layout, uint32_t w, uint32_t h, int kind)
{
    static uint64_t timeout_ns = 5000000000ull;
    if (!presenter_init() || beer_vulkan_device_lost()) return 0;
    if (!swapchain_ensure()) return 1;   /* no window right now: drop the frame, keep the game running */
    if (kind != 0 && !mid_ensure(w, h)) return 0;
    if (P.WaitForFences(P.device, 1, &P.fence, VK_TRUE, timeout_ns) != VK_SUCCESS) return 0;

    uint32_t index = 0;
    VkResult r = P.AcquireNextImageKHR(P.device, P.swapchain, 100000000ull, P.acquire_sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { P.generation = -1; return 1; }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return 1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { beer_vulkan_record_result(r, "acquire"); return 0; }

    P.ResetCommandBuffer(P.cb, 0);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    P.BeginCommandBuffer(P.cb, &begin);
    VkImage blit_src = source;
    VkImageLayout restore = layout;
    if (kind == 0 || kind == 1) {
        barrier(P.cb, source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    if (kind == 1) {
        barrier(P.cb, P.mid, P.mid_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { w, h, 1 } };
        P.CmdCopyImage(P.cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(P.cb, P.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit_src = P.mid;
        P.mid_initialized = 1;
    } else if (kind == 2) {
        barrier(P.cb, P.mid, P.mid_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
        P.CmdCopyBufferToImage(P.cb, P.stage, P.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(P.cb, P.mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit_src = P.mid;
        P.mid_initialized = 1;
    }
    barrier(P.cb, P.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)P.extent.width, (int32_t)P.extent.height, 1 } } };
    P.CmdBlitImage(P.cb, blit_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.images[index],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    {   /* the overlay copies write the same image: order them after the blit */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        P.CmdPipelineBarrier(P.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    record_overlay(P.cb, P.images[index]);
    barrier(P.cb, P.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (kind == 0 || kind == 1)
        barrier(P.cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restore, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    P.EndCommandBuffer(P.cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &P.acquire_sem,
        .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &P.cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &P.render_done[index] };
    P.ResetFences(P.device, 1, &P.fence);
    beer_vulkan_queue_lock();
    r = P.QueueSubmit(P.queue, 1, &submit, P.fence);
    if (r == VK_SUCCESS) {
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &P.render_done[index], .swapchainCount = 1, .pSwapchains = &P.swapchain, .pImageIndices = &index };
        VkResult pr = P.QueuePresentKHR(P.queue, &present);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) P.generation = -1;   /* SUBOPTIMAL just means the compositor rotates for us */
    }
    beer_vulkan_queue_unlock();
    if (r != VK_SUCCESS) { beer_vulkan_record_result(r, "present submit"); return 0; }
    /* No wait here: the next present waits for this fence before reusing the command buffer, so the CPU can record
     * the next frame while the GPU finishes this one. The source is a swapchain back buffer, which D3D12 lets the
     * game release only after it has waited for the GPU itself. */
    return 1;
}

int xwayland_window_present_image(uint64_t image, uint32_t layout, int width, int height, int direct_blit)
{
    if (!image || width <= 0 || height <= 0) return 0;
    return present_common((VkImage)(uintptr_t)image, (VkImageLayout)layout, (uint32_t)width, (uint32_t)height,
                          direct_blit ? 0 : 1);
}

int xwayland_window_present_rgba8(const uint8_t *pixels, int width, int height, int row_pitch)
{
    if (!pixels || width <= 0 || height <= 0 || row_pitch < width * 4) return 0;
    if (!presenter_init()) return 0;
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;
    if (!stage_ensure(size)) return 0;
    for (int y = 0; y < height; ++y) memcpy(P.stage_map + (size_t)y * width * 4, pixels + (size_t)y * row_pitch, (size_t)width * 4);
    return present_common(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, (uint32_t)width, (uint32_t)height, 2);
}

int xwayland_window_present_resource_rgba8(const void *resource, uint64_t serial, const uint8_t *pixels,
                                           int width, int height, int row_pitch)
{
    (void)resource; (void)serial;
    return xwayland_window_present_rgba8(pixels, width, height, row_pitch);
}

void xwayland_window_destroy(void)
{
    swapchain_destroy();
    pthread_mutex_lock(&A.lock);
    A.created = 0;
    pthread_mutex_unlock(&A.lock);
}
