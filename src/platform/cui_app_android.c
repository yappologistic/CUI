/* Android backend. NativeActivity (android_native_app_glue) for the window
 * and input queue, EGL for OpenGL ES, ANativeWindow locking for the software
 * renderer. JNI for the few things the NDK C API cannot do (opening URLs,
 * soft-keyboard characters). Touch maps to the mouse: first finger = left
 * button. The whole CUI loop runs inside android_main, which IS the app's
 * UI thread as far as the glue is concerned. */
#include "cui_platform.h"

#include <android/native_activity.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <jni.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

typedef struct android_app android_app;

typedef struct {
    struct android_app *act;
    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
    EGLSurface egl_surf;
    EGLConfig egl_cfg;
    int width, height;     /* window size in pixels */
    float scale;
    int down_x, down_y;    /* last finger position in window pixels */
    bool has_pointer;
    int tracked_id;        /* motion pointer id that owns the mouse (-1 none) */
    bool vsync;            /* swap interval to reapply when the surface is recreated */
    bool kb_shown;         /* soft keyboard requested for the current editing session */
    int ges_w, ges_h;      /* window size when the current gesture went down */
} cui__android;

static cui__android *cui__a(cui_app *app) { return app->plat; }

/* set by android_main before cui__plat_init runs */
static android_app *cui__android_activity;

double cui__plat_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char *cui__plat_name(void) { return "android"; }

int cui__plat_system_dark(void) { return -1; }

/* ---- JNI helpers ---- */

static JNIEnv *cui__android_jni(struct android_app *act) {
    JNIEnv *env = NULL;
    if ((*act->activity->vm)->AttachCurrentThread(act->activity->vm, &env, NULL) == JNI_OK) return env;
    return NULL;
}

bool cui__plat_open_url(const char *url) {
    android_app *act = cui__android_activity;
    if (!act) return false;
    JNIEnv *env = cui__android_jni(act);
    if (!env) return false;
    bool ok = false;
    jstring jurl = (*env)->NewStringUTF(env, url);
    jclass uri_cls = (*env)->FindClass(env, "android/net/Uri");
    jmethodID parse = uri_cls ? (*env)->GetStaticMethodID(env, uri_cls, "parse",
        "(Ljava/lang/String;)Landroid/net/Uri;") : NULL;
    jobject juri = (uri_cls && parse) ? (*env)->CallStaticObjectMethod(env, uri_cls, parse, jurl) : NULL;
    jclass intent_cls = juri ? (*env)->FindClass(env, "android/content/Intent") : NULL;
    jmethodID new_intent = intent_cls ? (*env)->GetMethodID(env, intent_cls, "<init>",
        "(Ljava/lang/String;Landroid/net/Uri;)V") : NULL;
    if (intent_cls && new_intent) {
        jstring action = (*env)->NewStringUTF(env, "android.intent.action.VIEW");
        jobject intent = (*env)->NewObject(env, intent_cls, new_intent, action, juri);
        if (intent) {
            jclass clazz = (*env)->GetObjectClass(env, act->activity->clazz);
            jmethodID start = (*env)->GetMethodID(env, clazz, "startActivity",
                "(Landroid/content/Intent;)V");
            if (start) (*env)->CallVoidMethod(env, act->activity->clazz, start, intent);
            (*env)->DeleteLocalRef(env, clazz);
        }
        /* ActivityNotFoundException (nothing handles the URL) or a lookup
         * failure clears to ok=false; only an unexceptional launch is true */
        if (!(*env)->ExceptionCheck(env)) ok = true;
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, action);
        if (intent) (*env)->DeleteLocalRef(env, intent);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (juri) (*env)->DeleteLocalRef(env, juri);
    if (uri_cls) (*env)->DeleteLocalRef(env, uri_cls);
    if (intent_cls) (*env)->DeleteLocalRef(env, intent_cls);
    (*env)->DeleteLocalRef(env, jurl);
    return ok;
}

bool cui__plat_file_dialog(cui_app *app, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)app; (void)o;
    if (cap) out[0] = 0;
    return false;
}

/* ---- clipboard: ClipboardManager over JNI ---- */

static char *cui__android_clip_buf;   /* get_clipboard result, freed on next read */

/* UTF-16 -> standard UTF-8 (handles surrogate pairs), like nativeCommitText */
static char *cui__android_utf16_to_utf8(JNIEnv *env, jstring js) {
    jsize len = (*env)->GetStringLength(env, js);
    const jchar *u16 = (*env)->GetStringChars(env, js, NULL);
    if (!u16) return NULL;
    char *utf8 = malloc((size_t)len * 4 + 1);
    if (utf8) {
        int n = 0;
        for (jsize i = 0; i < len; ) {
            unsigned int c = u16[i++];
            if (c >= 0xD800 && c <= 0xDBFF && i < len && u16[i] >= 0xDC00 && u16[i] <= 0xDFFF)
                c = 0x10000 + ((c - 0xD800) << 10) + (u16[i++] - 0xDC00);
            else if (c >= 0xD800 && c <= 0xDFFF)
                continue; /* lone surrogate: skip */
            if (c < 0x80) utf8[n++] = (char)c;
            else if (c < 0x800) { utf8[n++] = (char)(0xC0 | (c >> 6)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { utf8[n++] = (char)(0xE0 | (c >> 12)); utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
            else { utf8[n++] = (char)(0xF0 | (c >> 18)); utf8[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
        }
        utf8[n] = 0;
    }
    (*env)->ReleaseStringChars(env, js, u16);
    return utf8;
}

static void cui__android_set_clipboard(void *user, const char *utf8) {
    (void)user;
    android_app *act = cui__android_activity;
    if (!act || !utf8) return;
    JNIEnv *env = cui__android_jni(act);
    if (!env) return;
    /* NewStringUTF expects modified UTF-8 and mangles supplementary code
     * points; NewString takes UTF-16, so count then convert */
    size_t nchars = 0;
    for (const char *p = utf8; *p; ) {
        unsigned char b = (unsigned char)*p;
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        p += len;
        nchars += len == 4 ? 2 : 1;
    }
    jchar *u16 = malloc((nchars + 1) * sizeof *u16);
    if (!u16) return;
    size_t w = 0;
    for (const char *p = utf8; *p; ) {
        unsigned char b = (unsigned char)*p;
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        unsigned int c = 0;
        if (len == 1) c = *p++;
        else if (len == 2) { c = (*p++ & 0x1F) << 6; c |= *p++ & 0x3F; }
        else if (len == 3) { c = (*p++ & 0x0F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        else { c = (*p++ & 0x07) << 18; c |= (*p++ & 0x3F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        if (c >= 0x10000) {
            c -= 0x10000;
            u16[w++] = (jchar)(0xD800 + (c >> 10));
            u16[w++] = (jchar)(0xDC00 + (c & 0x3FF));
        } else {
            u16[w++] = (jchar)c;
        }
    }
    jstring jtext = (*env)->NewString(env, u16, (jsize)w);
    free(u16);
    if (!jtext || (*env)->ExceptionCheck(env)) { if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); return; }
    jclass ctx_cls = (*env)->GetObjectClass(env, act->activity->clazz);
    jfieldID svc_field = (*env)->GetStaticFieldID(env, ctx_cls, "CLIPBOARD_SERVICE",
        "Ljava/lang/String;");
    jstring svc = svc_field ? (*env)->GetStaticObjectField(env, ctx_cls, svc_field) : NULL;
    jmethodID get_sys = svc ? (*env)->GetMethodID(env, ctx_cls, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    jobject cm = (svc && get_sys) ? (*env)->CallObjectMethod(env, act->activity->clazz, get_sys, svc) : NULL;
    if (cm) {
        jclass cm_cls = (*env)->GetObjectClass(env, cm);
        jmethodID set = (*env)->GetMethodID(env, cm_cls, "setPrimaryClip",
            "(Landroid/content/ClipData;)V");
        jclass cd_cls = (*env)->FindClass(env, "android/content/ClipData");
        jmethodID new_cd = cd_cls ? (*env)->GetStaticMethodID(env, cd_cls, "newPlainText",
            "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;") : NULL;
        if (set && new_cd) {
            jobject cd = (*env)->CallStaticObjectMethod(env, cd_cls, new_cd, NULL, jtext);
            if (cd && !(*env)->ExceptionCheck(env))
                (*env)->CallVoidMethod(env, cm, set, cd);
            if (cd) (*env)->DeleteLocalRef(env, cd);
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (cd_cls) (*env)->DeleteLocalRef(env, cd_cls);
        (*env)->DeleteLocalRef(env, cm_cls);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, ctx_cls);
    if (svc) (*env)->DeleteLocalRef(env, svc);
    if (cm) (*env)->DeleteLocalRef(env, cm);
    (*env)->DeleteLocalRef(env, jtext);
}

static const char *cui__android_get_clipboard(void *user) {
    (void)user;
    android_app *act = cui__android_activity;
    if (!act) return "";
    JNIEnv *env = cui__android_jni(act);
    if (!env) return "";
    /* PushPopLocalFrame discards every local ref created inside with one pop,
     * so no class/method/string leaks accumulate per clipboard read */
    if ((*env)->PushLocalFrame(env, 16) != 0) return "";
    jclass ctx_cls = (*env)->GetObjectClass(env, act->activity->clazz);
    jfieldID svc_field = (*env)->GetStaticFieldID(env, ctx_cls, "CLIPBOARD_SERVICE",
        "Ljava/lang/String;");
    jstring svc = svc_field ? (*env)->GetStaticObjectField(env, ctx_cls, svc_field) : NULL;
    jmethodID get_sys = svc ? (*env)->GetMethodID(env, ctx_cls, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    jobject cm = (svc && get_sys) ? (*env)->CallObjectMethod(env, act->activity->clazz, get_sys, svc) : NULL;
    char *result = NULL;
    if (cm) {
        jclass cm_cls = (*env)->GetObjectClass(env, cm);
        jmethodID get_primary = (*env)->GetMethodID(env, cm_cls, "getPrimaryClip",
            "()Landroid/content/ClipData;");
        jobject cd = get_primary ? (*env)->CallObjectMethod(env, cm, get_primary) : NULL;
        if (cd) {
            jmethodID item_at = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, cd),
                "getItemAt", "(I)Landroid/content/ClipData$Item;");
            jmethodID count = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, cd), "getItemCount", "()I");
            jint n = count ? (*env)->CallIntMethod(env, cd, count) : 0;
            jobject item = (item_at && n > 0) ? (*env)->CallObjectMethod(env, cd, item_at, (jint)0) : NULL;
            if (item) {
                jmethodID get_text = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, item),
                    "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;");
                jobject text = get_text ? (*env)->CallObjectMethod(env, item, get_text,
                    act->activity->clazz) : NULL;
                if (text) {
                    jmethodID to_str = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, text),
                        "toString", "()Ljava/lang/String;");
                    jstring jstr = to_str ? (*env)->CallObjectMethod(env, text, to_str) : NULL;
                    if (jstr && !(*env)->ExceptionCheck(env)) {
                        /* GetStringUTFChars is modified UTF-8 (emoji become
                         * 6-byte surrogate encodings); read UTF-16 and convert
                         * to standard UTF-8. Heap-allocated, no 64 KiB cap —
                         * cui cores at 8 MiB on its own. */
                        result = cui__android_utf16_to_utf8(env, jstr);
                    }
                }
            }
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    free(cui__android_clip_buf);
    cui__android_clip_buf = result;
    return result ? result : "";
}

/* ---- soft keyboard ---- */

static cui_key cui__android_key(int32_t code); /* defined below; used by the textq drain */

/* commitText (swipe typing, accents, emoji) never reaches the NativeActivity
 * input queue and the C API cannot read the characters, so CuiActivity
 * forwards it over JNI from the UI thread; cui__plat_wait drains it on the
 * glue thread each frame. A committed string is prefixed with '\x01' so a
 * text whose first byte is a tag byte ("\u00043" pasted from the IME) cannot
 * be mistaken for a command; command items start '\b', '\x02', '\x03', '\x04'. */
static struct {
    pthread_mutex_t lock;
    char **items;
    int head, tail, cap;   /* ring of `cap` slots; cap-1 in use */
} cui__android_textq = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0 };

/* grows the ring instead of dropping items: a lost deletion or a lost
 * composition rewrite desyncs the screen from the IME with no recovery */
static bool cui__android_textq_push(const char *utf8) {
    bool ok = false;
    pthread_mutex_lock(&cui__android_textq.lock);
    int cap = cui__android_textq.cap;
    int used = cap ? (cui__android_textq.tail - cui__android_textq.head + cap) % cap : 0;
    if (used == cap - 1 || cap == 0) {
        int newcap = cap ? cap * 2 : 16;
        /* separate allocation: the wrapped ring must be copied in logical
         * order, which realloc cannot do in place */
        char **grown = malloc((size_t)newcap * sizeof *grown);
        if (grown) {
            for (int i = 0; i < used; i++)
                grown[i] = cui__android_textq.items[(cui__android_textq.head + i) % cap];
            free(cui__android_textq.items);
            cui__android_textq.items = grown;
            cui__android_textq.head = 0;
            cui__android_textq.tail = used;
            cui__android_textq.cap = newcap;
            cap = newcap;
        }
    }
    if (used < cap - 1) {
        char *dup = strdup(utf8);
        if (dup) {
            cui__android_textq.items[cui__android_textq.tail] = dup;
            cui__android_textq.tail = (cui__android_textq.tail + 1) % cui__android_textq.cap;
            ok = true;
        }
    }
    pthread_mutex_unlock(&cui__android_textq.lock);
    /* JNI pushes arrive on the UI thread and do not wake the glue thread's
     * ALooper; without this the text sits in the queue until the next poll
     * timeout (cursor blink), visibly delaying it */
    ALooper_wake(cui__android_activity->looper);
    return ok;
}

static void cui__android_textq_drain(cui_app *app) {
    for (;;) {
        pthread_mutex_lock(&cui__android_textq.lock);
        /* occupancy, not a null sentinel: a slot can only be empty because
         * the drain nulled it, which head!=tail already excludes */
        if (cui__android_textq.head == cui__android_textq.tail) {
            pthread_mutex_unlock(&cui__android_textq.lock);
            return;
        }
        char *item = cui__android_textq.items[cui__android_textq.head];
        cui__android_textq.items[cui__android_textq.head] = NULL;
        cui__android_textq.head = (cui__android_textq.head + 1) % cui__android_textq.cap;
        pthread_mutex_unlock(&cui__android_textq.lock);
        if (*item == '\b') {
            /* deleteSurroundingText outside the composing span */
            cui_input_key(app->ui, CUI_KEY_BACKSPACE, true, 0);
            cui_input_key(app->ui, CUI_KEY_BACKSPACE, false, 0);
        } else if (*item == '\x02') {
            /* the Java side replaced its composing span: delete the chars it
             * had us type for the old one */
            int n = atoi(item + 1);
            for (int i = 0; i < n; i++) {
                cui_input_key(app->ui, CUI_KEY_BACKSPACE, true, 0);
                cui_input_key(app->ui, CUI_KEY_BACKSPACE, false, 0);
            }
        } else if (*item == '\x03') {
            /* soft-keyboard key event (backspace, arrows, enter) forwarded
             * from CuiActivity.sendKeyEvent — these never reach the native
             * input queue */
            int code = atoi(item + 1);
            cui_key k = cui__android_key(code);
            if (k != CUI_KEY_NONE) {
                cui_input_key(app->ui, k, true, 0);
                cui_input_key(app->ui, k, false, 0);
            }
        } else if (*item == '\x04') {
            /* deleteSurroundingText(afterLength): a forward delete of n
             * codepoints. kpresseds cap at 255 per key per frame (cui_core.c),
             * so a larger request is split: consume what a frame holds and
             * re-queue the remainder for the next drain */
            int n = atoi(item + 1);
            int batch = n < 255 ? n : 255;
            for (int i = 0; i < batch; i++) {
                cui_input_key(app->ui, CUI_KEY_DELETE, true, 0);
                cui_input_key(app->ui, CUI_KEY_DELETE, false, 0);
            }
            if (n > batch) {
                char tag[24];
                snprintf(tag, sizeof tag, "\x04%d", n - batch);
                cui__android_textq_push(tag);
            }
        } else if (*item == '\x01') {
            /* committed text: strip the tag byte so a pasted or composed
             * string starting with a command byte is typed verbatim */
            cui_input_text(app->ui, item + 1);
        } else if (*item) {
            /* legacy untranslated callers (native_app_glue synthesizers) */
            cui_input_text(app->ui, item);
        }
        free(item);
    }
}

/* JNI entry points for CuiActivity (org.cui.hello); default visibility so
 * the JVM can look them up by name */
__attribute__((visibility("default")))
void Java_org_cui_hello_CuiActivity_nativeCommitText(JNIEnv *env, jobject thiz, jstring text) {
    (void)thiz;
    if (!text) return;
    /* GetStringChars returns UTF-16; convert to standard UTF-8 ourselves —
     * GetStringUTFChars would give modified UTF-8 with 6-byte surrogate
     * encodings for emoji */
    char *utf8 = cui__android_utf16_to_utf8(env, text);
    if (utf8) {
        /* prefix so committed text can never collide with a command tag */
        size_t n = strlen(utf8);
        char *tagged = malloc(n + 2);
        if (tagged) {
            tagged[0] = '\x01';
            memcpy(tagged + 1, utf8, n + 1);
            cui__android_textq_push(tagged);
            free(tagged);
        }
        free(utf8);
    }
}

/* the Java side replaced its composing span: delete the n codepoints it
 * previously had us type (queued as a "\x02<n>" tag item) */
__attribute__((visibility("default")))
void Java_org_cui_hello_CuiActivity_nativeDeleteSpanCodepoints(JNIEnv *env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    char tag[24];
    snprintf(tag, sizeof tag, "\x02%d", (int)(n > 0 ? n : 0));
    cui__android_textq_push(tag);
}

__attribute__((visibility("default")))
void Java_org_cui_hello_CuiActivity_nativeDeleteChars(JNIEnv *env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    for (jint i = 0; i < n; i++) cui__android_textq_push("\b");
}

__attribute__((visibility("default")))
void Java_org_cui_hello_CuiActivity_nativeDeleteCharsForward(JNIEnv *env, jobject thiz, jint n) {
    (void)env; (void)thiz;
    char tag[24];
    snprintf(tag, sizeof tag, "\x04%d", (int)(n > 0 ? n : 0));
    cui__android_textq_push(tag);
}

/* soft-keyboard key event (backspace, arrows, enter) from sendKeyEvent;
 * queued as a "\x03<code>" tag item */
__attribute__((visibility("default")))
void Java_org_cui_hello_CuiActivity_nativeKeyEvent(JNIEnv *env, jobject thiz, jint code) {
    (void)env; (void)thiz;
    char tag[24];
    snprintf(tag, sizeof tag, "\x03%d", (int)code);
    cui__android_textq_push(tag);
}

/* The IME must be driven from the UI thread; CuiActivity.showKeyboard posts
 * there, and its CuiInputView is the text-editor view the IME connects to. */
static void cui__android_show_kb_reset(struct android_app *act) {
    JNIEnv *env = cui__android_jni(act);
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, act->activity->clazz);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, "resetComposition", "()V") : NULL;
    if (m && !(*env)->ExceptionCheck(env))
        (*env)->CallVoidMethod(env, act->activity->clazz, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
}

static void cui__android_show_kb(struct android_app *act, bool show) {
    JNIEnv *env = cui__android_jni(act);
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, act->activity->clazz);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, show ? "showKeyboard" : "hideKeyboard", "()V") : NULL;
    if (m && !(*env)->ExceptionCheck(env))
        (*env)->CallVoidMethod(env, act->activity->clazz, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
}

/* ---- lifecycle commands from the glue thread ---- */

static void cui__android_cmd(android_app *act, int32_t cmd) {
    cui_app *app = (cui_app *)act->userData;
    if (!app) return;
    cui__android *a = cui__a(app);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        ANativeWindow *w = act->window;
        a->width = ANativeWindow_getWidth(w);
        a->height = ANativeWindow_getHeight(w);
        /* the software renderer produces BGRA/RGBA8888 pixels */
        ANativeWindow_setBuffersGeometry(w, a->width, a->height, WINDOW_FORMAT_RGBA_8888);
        /* rotation (or any surface loss): the EGL surface died in TERM_WINDOW
         * while the context lived on — recreate it at the new size here, on
         * this thread, before the next frame presents */
        if (a->egl_ctx && !a->egl_surf) {
            if (cui__plat_gl_create(app, a->vsync)) {
                a->width = ANativeWindow_getWidth(w);
                a->height = ANativeWindow_getHeight(w);
            } else {
                /* gl_create may have made the surface before eglMakeCurrent
                 * failed; leave it unset so the next INIT_WINDOW retries */
                a->egl_surf = EGL_NO_SURFACE;
            }
        }
        cui_request_redraw(app->ui);
        break;
    }
    case APP_CMD_TERM_WINDOW:
        if (a->egl_surf) {
            if (a->egl_ctx) eglMakeCurrent(a->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(a->egl_dpy, a->egl_surf);
            a->egl_surf = EGL_NO_SURFACE;
        }
        break;
    case APP_CMD_GAINED_FOCUS:
        cui_input_focus(app->ui, true);
        /* the previous editor may still hold a composition: end it and
         * restart the IME so its composing text cannot reach the new one */
        cui__android_show_kb_reset(act);
        break;
    case APP_CMD_LOST_FOCUS: cui_input_focus(app->ui, false); break;
    case APP_CMD_DESTROY: app->running = false; break;
    default: break;
    }
}

static float cui__android_density(android_app *act) {
    AConfiguration *cfg = act->config;
    int dpi = cfg ? AConfiguration_getDensity(cfg) : 0;
    if (dpi <= 0) dpi = ACONFIGURATION_DENSITY_MEDIUM;
    float s = (float)dpi / 160.0f;
    const char *env = getenv("CUI_SCALE");
    if (env) { float e = (float)atof(env); if (e > 0.25f && e < 8) s = e; }
    if (s < 1.0f) s = 1.0f;
    return roundf(s * 4) / 4;
}

/* ---- input ---- */

static cui_key cui__android_key(int32_t code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return (cui_key)(CUI_KEY_A + (int)(code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return (cui_key)(CUI_KEY_0 + (int)(code - AKEYCODE_0));
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) return (cui_key)(CUI_KEY_F1 + (int)(code - AKEYCODE_F1));
    switch (code) {
    case AKEYCODE_TAB: return CUI_KEY_TAB;
    case AKEYCODE_ENTER: return CUI_KEY_ENTER;
    case AKEYCODE_ESCAPE: return CUI_KEY_ESCAPE;
    case AKEYCODE_DEL: return CUI_KEY_BACKSPACE;
    case AKEYCODE_FORWARD_DEL: return CUI_KEY_DELETE;
    case AKEYCODE_SPACE: return CUI_KEY_SPACE;
    case AKEYCODE_DPAD_LEFT: return CUI_KEY_LEFT;
    case AKEYCODE_DPAD_RIGHT: return CUI_KEY_RIGHT;
    case AKEYCODE_DPAD_UP: return CUI_KEY_UP;
    case AKEYCODE_DPAD_DOWN: return CUI_KEY_DOWN;
    case AKEYCODE_MOVE_HOME: case AKEYCODE_NUMPAD_7: return CUI_KEY_HOME;
    case AKEYCODE_MOVE_END: case AKEYCODE_NUMPAD_1: return CUI_KEY_END;
    case AKEYCODE_PAGE_UP: return CUI_KEY_PAGE_UP;
    case AKEYCODE_PAGE_DOWN: return CUI_KEY_PAGE_DOWN;
    case AKEYCODE_MINUS: return CUI_KEY_MINUS;
    case AKEYCODE_EQUALS: return CUI_KEY_EQUAL;
    case AKEYCODE_COMMA: return CUI_KEY_COMMA;
    case AKEYCODE_PERIOD: return CUI_KEY_PERIOD;
    case AKEYCODE_SLASH: return CUI_KEY_SLASH;
    default: return CUI_KEY_NONE;
    }
}

static int cui__android_mods(int32_t meta) {
    int m = 0;
    if (meta & AMETA_SHIFT_ON) m |= CUI_MOD_SHIFT;
    if (meta & AMETA_CTRL_ON) m |= CUI_MOD_CTRL;
    if (meta & AMETA_ALT_ON) m |= CUI_MOD_ALT;
    if (meta & AMETA_META_ON) m |= CUI_MOD_SUPER;
    return m;
}

static bool cui__android_key_event(cui_app *app, AInputEvent *ev) {
    cui_ctx *ui = app->ui;
    int32_t action = AKeyEvent_getAction(ev);
    bool down = action == AKEY_EVENT_ACTION_DOWN;
    int32_t meta = AKeyEvent_getMetaState(ev);
    int32_t code = AKeyEvent_getKeyCode(ev);
    /* IME edit actions (clipboard toolbar paste/copy/cut) arrive as these
     * key codes; map them onto the textedit Ctrl shortcuts */
    switch (code) {
    case AKEYCODE_PASTE:    code = AKEYCODE_V; meta |= AMETA_CTRL_ON; break;
    case AKEYCODE_COPY:     code = AKEYCODE_C; meta |= AMETA_CTRL_ON; break;
    case AKEYCODE_CUT:      code = AKEYCODE_X; meta |= AMETA_CTRL_ON; break;
    default: break;
    }
    cui_key k = cui__android_key(code);
    int mods = cui__android_mods(meta);
    bool handled = false;
    char text[8] = "";
    /* CuiActivity's InputConnection delivers soft-keyboard text via JNI;
     * synthesizing it here too would double every character. Soft-keyboard
     * events that survive the bridge are only keys the bridge never sees
     * (enter, arrows, and DEL — after a commit Gboard sends backspace as a
     * key event instead of deleteSurroundingText). */
    bool soft_del = AKeyEvent_getFlags(ev) & AKEY_EVENT_FLAG_SOFT_KEYBOARD
        && (code == AKEYCODE_DEL || code == AKEYCODE_FORWARD_DEL);
    if (down && (!(AKeyEvent_getFlags(ev) & AKEY_EVENT_FLAG_SOFT_KEYBOARD) || soft_del)
             && code != AKEYCODE_PASTE && code != AKEYCODE_COPY
             && code != AKEYCODE_CUT && !(mods & CUI_MOD_CTRL)) {
        /* soft-keyboard text: the code point is not exposed by the C API,
         * ask the Java KeyEvent (also for mapped keys, for shifted symbols) */
        android_app *act = cui__a(app)->act;
        JNIEnv *env = cui__android_jni(act);
        if (env) {
            jclass kc = (*env)->FindClass(env, "android/view/KeyEvent");
            jmethodID ctor = kc ? (*env)->GetMethodID(env, kc, "<init>", "(JJIIIIII)V") : NULL;
            /* deviceId/scancode: getUnicodeChar must use the physical
             * keyboard's map, not VIRTUAL_KEYBOARD (German Shift+2 etc.) */
            jobject jke = (kc && ctor && !(*env)->ExceptionCheck(env)) ? (*env)->NewObject(env, kc, ctor,
                (jlong)AKeyEvent_getDownTime(ev), (jlong)AKeyEvent_getEventTime(ev),
                (jint)action, (jint)AKeyEvent_getKeyCode(ev),
                (jint)AKeyEvent_getRepeatCount(ev),
                (jint)meta,
                (jint)AInputEvent_getDeviceId(ev),
                (jint)AKeyEvent_getScanCode(ev)) : NULL;
            if (jke) {
                jmethodID unicode = (*env)->GetMethodID(env, kc, "getUnicodeChar", "(I)I");
                jint ch = (unicode && !(*env)->ExceptionCheck(env))
                    ? (*env)->CallIntMethod(env, jke, unicode, (jint)meta) : 0;
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ch = 0; }
                if (ch > 0) {
                    int n = 0;
                    unsigned int c = (unsigned int)ch;
                    if (c < 0x80) text[n++] = (char)c;
                    else if (c < 0x800) { text[n++] = (char)(0xC0 | (c >> 6)); text[n++] = (char)(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) { text[n++] = (char)(0xE0 | (c >> 12)); text[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); text[n++] = (char)(0x80 | (c & 0x3F)); }
                    else { text[n++] = (char)(0xF0 | (c >> 18)); text[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); text[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); text[n++] = (char)(0x80 | (c & 0x3F)); }
                    text[n] = 0;
                }
                (*env)->DeleteLocalRef(env, jke);
            }
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            if (kc) (*env)->DeleteLocalRef(env, kc);
        }
    }
    if (k) {
        cui_input_key(ui, k, down, mods);
        handled = true;
    }
    if (down && text[0] && !(mods & CUI_MOD_CTRL)) {
        cui_input_text(ui, text);
        handled = true;
    }
    return handled;
}

static int32_t cui__android_win_xy[2];   /* last known window origin on screen */

static void cui__android_window_origin(cui__android *a) {
    /* the NDK exposes no window origin; ask the activity over JNI so
     * split-screen or offset windows map raw screen coordinates locally */
    android_app *act = a ? a->act : cui__android_activity;
    if (!act) return;
    JNIEnv *env = cui__android_jni(act);
    if (!env) return;
    /* receiver is a View (the surface view draws the content); on failure
     * the exception must be cleared here, outside the guarded call below,
     * or every later JNI call on this thread throws pending-exception */
    jclass cls = (*env)->GetObjectClass(env, act->activity->clazz);
    jmethodID loc = cls ? (*env)->GetMethodID(env, cls, "getLocationOnScreen", "([I)V") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!loc) { if (cls) (*env)->DeleteLocalRef(env, cls); return; }
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (!arr) { if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, cls); return; }
    (*env)->CallVoidMethod(env, act->activity->clazz, loc, arr);
    if (!(*env)->ExceptionCheck(env)) {
        jint *p = (*env)->GetIntArrayElements(env, arr, NULL);
        if (p) {
            cui__android_win_xy[0] = p[0];
            cui__android_win_xy[1] = p[1];
            (*env)->ReleaseIntArrayElements(env, arr, p, JNI_ABORT);
        }
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, arr);
    (*env)->DeleteLocalRef(env, cls);
}

static void cui__android_motion_event(cui_app *app, AInputEvent *ev) {
    cui__android *a = cui__a(app);
    cui_ctx *ui = app->ui;
    float s = a->scale;
    int32_t action = AMotionEvent_getAction(ev);
    int ptr_index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t ptr_id = AMotionEvent_getPointerId(ev, ptr_index);
    /* IME show/hide resizes the window (adjustResize) and the input system
     * remaps queued events into the NEW size, so mid-gesture coordinates are
     * off by the keyboard height. If a gesture is down and the window size it
     * started in no longer matches, drop everything after the down: release
     * the button at the down position and swallow the rest of the gesture. */
    if (a->has_pointer) {
        int w = a->act->window ? ANativeWindow_getWidth(a->act->window) : a->ges_w;
        int h = a->act->window ? ANativeWindow_getHeight(a->act->window) : a->ges_h;
        if (w != a->ges_w || h != a->ges_h) {
            uint32_t base = action & AMOTION_EVENT_ACTION_MASK;
            /* CANCEL must pass through even after the resize: it is the only
             * path that ends a gesture the input system abandons */
            if (base != AMOTION_EVENT_ACTION_UP && base != AMOTION_EVENT_ACTION_CANCEL) return;
            cui_input_mouse_move(ui, a->down_x / s, a->down_y / s);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
            a->has_pointer = false;
            a->tracked_id = -1;
            return;
        }
    }
    /* track the pointer that went down first so a second finger cannot
     * hijack the mouse position while it is on the screen */
    int idx = 0;
    switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
        a->tracked_id = (int)ptr_id;
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if ((int)ptr_id == a->tracked_id) {
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
            a->has_pointer = false;
            a->tracked_id = -1;
        }
        return;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        return;   /* the tracked pointer keeps owning the mouse */
    default: break;
    }
    for (size_t i = 0, n = AMotionEvent_getPointerCount(ev); i < n; i++) {
        if ((int)AMotionEvent_getPointerId(ev, i) == a->tracked_id) { idx = (int)i; break; }
    }
    /* window-relative X/Y are unreliable on this device: the input
     * dispatcher caches the frame from when the IME was open and keeps
     * remapping taps by the keyboard height long after it closed. Raw
     * screen coordinates bypass that remap; our window is top-anchored
     * (fullscreen, adjustResize shrinks only the bottom) so raw == window
     * in every state we draw. */
    int raw_idx = idx;
    if (AMotionEvent_getRawX(ev, idx) == 0 &&
        AMotionEvent_getRawX(ev, 0) != 0) raw_idx = 0;   /* per-pointer raw unsupported */
    /* raw coordinates are screen-relative; subtract the window origin
     * (refreshed per event, usually 0 for this top-anchored fullscreen
     * window) so split-screen windows map taps to their local positions */
    cui__android_window_origin(a);
    float x = (AMotionEvent_getRawX(ev, raw_idx) - (float)cui__android_win_xy[0]) / s;
    float y = (AMotionEvent_getRawY(ev, raw_idx) - (float)cui__android_win_xy[1]) / s;
    switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
        if (a->act->window) {
            a->ges_w = ANativeWindow_getWidth(a->act->window);
            a->ges_h = ANativeWindow_getHeight(a->act->window);
        }
        a->down_x = (int)(x * s);
        a->down_y = (int)(y * s);
        cui_input_mouse_move(ui, x, y);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        a->has_pointer = true;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        if (a->has_pointer) cui_input_mouse_move(ui, x, y);
        break;
    case AMOTION_EVENT_ACTION_UP:
        cui_input_mouse_move(ui, x, y);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        a->has_pointer = false;
        a->tracked_id = -1;
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        if (a->has_pointer) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        a->has_pointer = false;
        a->tracked_id = -1;
        break;
    default: break;
    }
}

static int32_t cui__android_input(android_app *act, AInputEvent *ev) {
    cui_app *app = (cui_app *)act->userData;
    if (!app) return 0;
    switch (AInputEvent_getType(ev)) {
    case AINPUT_EVENT_TYPE_MOTION:
        cui__android_motion_event(app, ev);
        return 1;
    case AINPUT_EVENT_TYPE_KEY:
        return cui__android_key_event(app, ev) ? 1 : 0;
    default: return 0;
    }
}

/* ---- EGL ---- */

static EGLConfig cui__android_pick_config(EGLDisplay dpy) {
    const EGLint attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg = NULL;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) return NULL;
    return cfg;
}

void *cui__plat_gl_proc(const char *name) { return eglGetProcAddress(name); }

bool cui__plat_gl_create(cui_app *app, bool vsync) {
    cui__android *a = cui__a(app);
    android_app *act = a->act;
    if (!act->window || a->egl_dpy == EGL_NO_DISPLAY) return false;
    a->vsync = vsync;
    if (a->egl_ctx) {
        /* surface recreated after the window came back; context is kept */
        if (a->egl_surf) eglDestroySurface(a->egl_dpy, a->egl_surf);
        a->egl_surf = eglCreateWindowSurface(a->egl_dpy, a->egl_cfg, act->window, NULL);
        if (a->egl_surf == EGL_NO_SURFACE) return false;
        if (!eglMakeCurrent(a->egl_dpy, a->egl_surf, a->egl_surf, a->egl_ctx)) {
            eglDestroySurface(a->egl_dpy, a->egl_surf);
            a->egl_surf = EGL_NO_SURFACE;
            return false;
        }
        eglSwapInterval(a->egl_dpy, vsync ? 1 : 0);
        return true;
    }
    a->egl_cfg = cui__android_pick_config(a->egl_dpy);
    if (!a->egl_cfg) return false;
    static const EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    a->egl_ctx = eglCreateContext(a->egl_dpy, a->egl_cfg, EGL_NO_CONTEXT, attrs);
    if (a->egl_ctx == EGL_NO_CONTEXT) return false;
    a->egl_surf = eglCreateWindowSurface(a->egl_dpy, a->egl_cfg, act->window, NULL);
    if (a->egl_surf == EGL_NO_SURFACE) { eglDestroyContext(a->egl_dpy, a->egl_ctx); a->egl_ctx = EGL_NO_CONTEXT; return false; }
    if (!eglMakeCurrent(a->egl_dpy, a->egl_surf, a->egl_surf, a->egl_ctx)) {
        eglDestroySurface(a->egl_dpy, a->egl_surf); a->egl_surf = EGL_NO_SURFACE;
        eglDestroyContext(a->egl_dpy, a->egl_ctx); a->egl_ctx = EGL_NO_CONTEXT;
        return false;
    }
    eglSwapInterval(a->egl_dpy, vsync ? 1 : 0);
    return true;
}

void cui__plat_gl_release(cui_app *app) {
    cui__android *a = cui__a(app);
    if (a->egl_surf) { eglDestroySurface(a->egl_dpy, a->egl_surf); a->egl_surf = EGL_NO_SURFACE; }
    if (a->egl_ctx) { eglMakeCurrent(a->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(a->egl_dpy, a->egl_ctx); a->egl_ctx = EGL_NO_CONTEXT; }
}

void cui__plat_gl_swap(cui_app *app) {
    cui__android *a = cui__a(app);
    if (a->egl_surf) eglSwapBuffers(a->egl_dpy, a->egl_surf);
}

/* ---- software presentation: copy into the window's buffer ---- */

static void cui__android_blit(cui_app *app, const void *bgra, int w, int h, int stride, int px, int py, int pw, int ph) {
    ANativeWindow_Buffer buf;
    if (!app || !cui__a(app)->act || !cui__a(app)->act->window) return;
    if (ANativeWindow_lock(cui__a(app)->act->window, &buf, NULL) != 0) return;
    if (buf.format == WINDOW_FORMAT_RGBA_8888) {
        uint8_t *dst = (uint8_t *)buf.bits;
        const uint8_t *src = (const uint8_t *)bgra;
        int cw = pw < buf.width - px ? pw : buf.width - px;
        int ch = ph < buf.height - py ? ph : buf.height - py;
        if (cw > 0 && ch > 0) {
            if (px == 0 && cw == w && buf.stride == w && stride == w * 4) {
                /* full rows: the software renderer's BGRA has to become the
                 * window's RGBA, so bytes cannot just be copied */
                for (int y = 0; y < ch; y++) {
                    const uint8_t *s = src + (size_t)(py + y) * stride;
                    uint8_t *d = dst + (size_t)(py + y) * buf.stride * 4;
                    for (int x4 = 0; x4 < cw * 4; x4 += 4) {
                        d[x4] = s[x4 + 2]; d[x4 + 1] = s[x4 + 1]; d[x4 + 2] = s[x4]; d[x4 + 3] = s[x4 + 3];
                    }
                }
            } else {
                for (int y = 0; y < ch; y++) {
                    const uint8_t *s = src + (size_t)(py + y) * stride + (size_t)px * 4;
                    uint8_t *d = dst + (size_t)(py + y) * buf.stride * 4 + (size_t)px * 4;
                    for (int x4 = 0; x4 < cw * 4; x4 += 4) {
                        d[x4] = s[x4 + 2]; d[x4 + 1] = s[x4 + 1]; d[x4 + 2] = s[x4]; d[x4 + 3] = s[x4 + 3];
                    }
                }
            }
        }
    }
    ANativeWindow_unlockAndPost(cui__a(app)->act->window);
}

void cui__plat_present(cui_app *app, const void *bgra, int w, int h, int stride) {
    cui__android_blit(app, bgra, w, h, stride, 0, 0, w, h);
}

bool cui__plat_present_part(cui_app *app, const void *bgra, int w, int h, int stride, int px, int py, int pw, int ph) {
    cui__android_blit(app, bgra, w, h, stride, px, py, pw, ph);
    return true;
}

/* ---- init / shutdown ---- */

bool cui__plat_init(cui_app *app) {
    if (!cui__android_activity) return false;
    cui__android *a = calloc(1, sizeof *a);
    if (!a) return false;
    app->plat = a;
    a->act = cui__android_activity;
    a->act->userData = app;
    a->act->onAppCmd = cui__android_cmd;
    a->act->onInputEvent = cui__android_input;
    a->tracked_id = -1;
    a->scale = cui__android_density(a->act);
    a->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (a->egl_dpy != EGL_NO_DISPLAY && eglInitialize(a->egl_dpy, NULL, NULL)) {
        eglBindAPI(EGL_OPENGL_ES_API);
    } else {
        a->egl_dpy = EGL_NO_DISPLAY;
    }
    cui_platform_iface pi;
    memset(&pi, 0, sizeof pi);
    pi.user = app;
    pi.set_clipboard = cui__android_set_clipboard;
    pi.get_clipboard = cui__android_get_clipboard;
    cui_set_platform(app->ui, &pi);
    /* block until the surface exists so the first frame has a size */
    int events = 0;
    struct android_poll_source *src = NULL;
    while (!a->act->destroyRequested && a->act->window == NULL) {
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0 && src)
            src->process(a->act, src);
    }
    if (a->act->destroyRequested) { app->plat = NULL; free(a); return false; }
    a->width = ANativeWindow_getWidth(a->act->window);
    a->height = ANativeWindow_getHeight(a->act->window);
    return true;
}

void cui__plat_show(cui_app *app) { (void)app; }
void cui__plat_set_title(cui_app *app, const char *title) { (void)app; (void)title; }
void cui__plat_set_size(cui_app *app, int w, int h) { (void)app; (void)w; (void)h; }
void *cui__plat_native(cui_app *app) { return (void *)cui__a(app)->act->window; }

void cui__plat_shutdown(cui_app *app) {
    cui__android *a = cui__a(app);
    if (!a) return;
    cui__plat_gl_release(app);
    if (a->egl_dpy != EGL_NO_DISPLAY) eglTerminate(a->egl_dpy);
    free(a);
    app->plat = NULL;
}

double cui__plat_refresh_hz(cui_app *app) {
    (void)app;
    return 0;   /* EGL does not expose it; the app loop paces by the clock */
}

/* one fullscreen window: no window management, nothing to change or report */
void cui__plat_maximize(cui_app *app) { (void)app; }
void cui__plat_minimize(cui_app *app) { (void)app; }
void cui__plat_restore(cui_app *app) { (void)app; }
void cui__plat_set_position(cui_app *app, int x, int y) { (void)app; (void)x; (void)y; }

cui_window_state cui__plat_window_state(cui_app *app) {
    (void)app;
    return CUI_WINDOW_NORMAL;
}

void cui__plat_placement(cui_app *app, cui_window_placement *out) {
    cui__android *a = cui__a(app);
    out->x = 0;
    out->y = 0;
    out->width = (int)((float)a->width / a->scale + 0.5f);
    out->height = (int)((float)a->height / a->scale + 0.5f);
    out->state = CUI_WINDOW_NORMAL;
    out->has_position = false;
}

void cui__plat_metrics(cui_app *app, float *w, float *h, float *scale, int *fb_w, int *fb_h) {
    cui__android *a = cui__a(app);
    ANativeWindow *win = a->act->window;
    if (win) {
        a->width = ANativeWindow_getWidth(win);
        a->height = ANativeWindow_getHeight(win);
    }
    *scale = a->scale;
    *fb_w = a->width; *fb_h = a->height;
    *w = (float)a->width / a->scale;
    *h = (float)a->height / a->scale;
}

void cui__plat_wait(cui_app *app, double timeout) {
    cui__android *a = cui__a(app);
    /* show/hide the soft keyboard when a text edit gains or loses focus; the
     * latch avoids spamming the IME every frame */
    bool wants = cui_wants_keyboard(app->ui);
    if (wants && !a->kb_shown) {
        a->kb_shown = true;
        cui__android_show_kb(a->act, true);
    } else if (!wants && a->kb_shown) {
        a->kb_shown = false;
        cui__android_show_kb(a->act, false);
    }
    int events = 0;
    struct android_poll_source *src = NULL;
    int ms = timeout < 0 ? -1 : (int)(timeout * 1000);
    int r = ALooper_pollOnce(ms, NULL, &events, (void **)&src);
    if (r >= 0 && src) src->process(a->act, src);
    cui__android_textq_drain(app);
    if (a->act->destroyRequested) app->running = false;
}

/* The glue thread runs android_main; the app's own main() (which calls
 * cui_app_run and owns the poll loop from there) takes over on that thread. */
int main(int argc, char **argv);

void android_main(struct android_app *state) {
    cui__android_activity = state;
    main(0, NULL);
}
