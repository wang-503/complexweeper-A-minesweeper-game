// 安卓平台层：NativeActivity + native_app_glue + EGL/GLES2。
//
// 这一层只做"安卓才需要做的事"，游戏逻辑与界面全在 ui.c / game.c 里，
// 所以宿主机上能跑同一份界面代码出图自检（见 main_host.c）。
//
// 渲染路径刻意做得最笨：
//   1. 软件把整帧画进一块 BGRA 帧缓冲（ui.c）
//   2. 上传成一张纹理
//   3. 用一个全屏四边形贴出去（GL_NEAREST，保持像素风）
// 手机分辨率下这块帧缓冲只有几 MB，一次上传，没什么可优化的。
#include <android/native_window.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/native_activity.h>
#include <jni.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "font.h"
#include "game.h"
#include "render.h"
#include "strbuf.h"
#include "ui.h"
#include "ui_strings.h"
#include "frame_buf.h"

#define LOG_TAG "complexsweeper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 帧缓冲上限：专家盘在缩放 1 下的布局约 570×1290，3000×3000 已经富余很多。
// 手机上这块内存约 36MB，属于可接受范围（用完即放，不重复申请）。
#define FB_MAX_W 3000
#define FB_MAX_H 3000

// 全屏四边形的两个三角形（顺序：两个三角形拼一个矩形，UV 的 v 轴翻转，
// 因为 GL 的纹理坐标原点在左下，而我们的帧缓冲第一行是屏幕顶部）
static const float QUAD_VERTICES[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
    1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 0.0f,
};

static const char *VERTEX_SRC =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vUV = aUV;\n"
    "}\n";

static const char *FRAGMENT_SRC =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vUV);\n"
    "}\n";

typedef struct {
    struct android_app *app;
    bool has_window;
    bool focused;
    bool animating;
    bool need_frame;         // 有一帧欠着没画（输入/尺寸变化都会置起来）
    int32_t cell_px;         // 单格边长（设备像素），由 0.7cm × 屏幕密度算出
    uint32_t paused_at_ms;   // 失焦时刻（0 = 不在暂停中）
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int32_t width;      // 窗口像素尺寸
    int32_t height;
    GLuint program;
    GLuint texture;
    GLint a_pos;
    GLint a_uv;
    bool gl_ok;                  // 着色器/纹理准备成功才绘制；失败时留日志而不是画黑的
    bool logged_first_frame;
    // 帧缓冲：尺寸永远等于窗口（1:1 上屏，没有缩放层）
    uint32_t *fb;
    int32_t fb_w;
    int32_t fb_h;
    // 图集与字形（从 assets 读进来后一直留着）
    uint8_t *atlas;
    size_t atlas_len;
    uint8_t *font;
    size_t font_len;
    Font font_ctx;
    Ui ui;
    // 触摸
    int32_t touch_id;
    bool touch_active;
    int32_t touch_x, touch_y;
    uint32_t touch_down_ms;
    uint32_t now_ms;
} App;

static uint32_t tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000));
}

// ---------------------------------------------------------------- assets
static bool read_asset(AAssetManager *mgr, const char *name, uint8_t **out, size_t *out_len) {
    AAsset *a = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!a) {
        LOGE("打不开 assets/%s", name);
        return false;
    }
    const off_t n = AAsset_getLength(a);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        AAsset_close(a);
        return false;
    }
    const int got = AAsset_read(a, buf, (size_t)n);
    AAsset_close(a);
    if (got != (int)n) {
        LOGE("读 assets/%s 只读到 %d / %lld 字节", name, got, (long long)n);
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = (size_t)n;
    LOGI("assets/%s %zu 字节", name, *out_len);
    return true;
}

// ---------------------------------------------------------------- GL
static GLuint compile_shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("着色器编译失败: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool gl_setup(App *app) {
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) return false;
    app->program = glCreateProgram();
    glAttachShader(app->program, vs);
    glAttachShader(app->program, fs);
    glLinkProgram(app->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(app->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(app->program, sizeof log, NULL, log);
        LOGE("着色器链接失败: %s", log);
        return false;
    }
    app->a_pos = glGetAttribLocation(app->program, "aPos");
    app->a_uv = glGetAttribLocation(app->program, "aUV");
    glGenTextures(1, &app->texture);
    glBindTexture(GL_TEXTURE_2D, app->texture);
    // 最近邻 + 不重复：像素风的关键，也是"整数倍放大"看起来干净的原因
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    app->gl_ok = true;
    LOGI("GL 就绪：version=%s renderer=%s",
         (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return true;
}

static void gl_teardown(App *app) {
    if (app->program) { glDeleteProgram(app->program); app->program = 0; }
    if (app->texture) { glDeleteTextures(1, &app->texture); app->texture = 0; }
    app->gl_ok = false;
    app->logged_first_frame = false;
}

// 帧缓冲尺寸 = 窗口尺寸，1:1 上屏（**没有缩放层**）。
// 于是布局坐标就是设备像素，触摸坐标与绘制天然同源；单格边长直接由物理尺寸
// （0.7cm × 屏幕密度）算出来，不存在"物理尺寸 → 整数倍率"的取整。
// HUD 缩放（计雷器/人脸/菜单栏）由 ui_refresh_scale 单独挑，与单格尺寸解耦。
static void rebuild_framebuffer(App *app) {
    if (app->width < 1) app->width = 1;
    if (app->height < 1) app->height = 1;

    // **界面布局必须跟着窗口尺寸走**。
    // 这里漏过一步很严重的：ui_init 只给了一个保守的默认尺寸（1920x1080），
    // 而平台层从来没调过 ui_resize —— 于是任何"不是 1920x1080"的设备上
    // 布局都按 1920x1080 算，而帧缓冲是真实窗口尺寸：
    // 内容被画在左上角一块，右边/下边留一大片黑（设备实测：
    // 2712x1220 的机器上只有左上 1920x1080 有内容，右侧 792 列纯黑）。
    // 所以每次重建帧缓冲都要先同步窗口尺寸，再算布局。
    ui_resize(&app->ui, app->width, app->height);

    // 调试用：CSB_FORCE_CELL 直接钉住单格像素数（只读环境变量，不写文件）
    {
        const char *e = getenv("CSB_FORCE_CELL");
        if (e) {
            const int32_t c = atoi(e);
            if (c >= 1 && c <= 512) app->ui.cell_px = c;
        }
    }
    ui_set_cell_px(&app->ui, app->cell_px);

    if (app->fb == NULL || app->fb_w != app->width || app->fb_h != app->height) {
        free(app->fb);
        app->fb = (uint32_t *)calloc((size_t)app->width * (size_t)app->height, 4);
        if (!app->fb) {
            LOGE("帧缓冲分配失败 %dx%d", app->width, app->height);
            app->fb_w = 0;
            app->fb_h = 0;
            return;
        }
        app->fb_w = app->width;
        app->fb_h = app->height;
    }
    const UiLayout L = ui_layout(&app->ui);
    LOGI("窗口 %dx%d，单格 %dpx，HUD %d×，侧栏 %dpx，可见 %d×%d 格，盘 %ux%u，pan=(%d,%d)",
         app->width, app->height, L.cell, L.hud, L.side_w, L.vis_cols, L.vis_rows,
         app->ui.game.w, app->ui.game.h, L.pan_x, L.pan_y);
}

// 屏幕坐标 → 界面坐标。帧缓冲 == 窗口，所以是恒等映射。
// 保留成函数是为了将来若真要加缩放，只有这一处需要改。
static void screen_to_ui(const App *app, int32_t sx, int32_t sy, int32_t *ux, int32_t *uy) {
    (void)app;
    *ux = sx;
    *uy = sy;
}
static void draw_frame(App *app) {
    if (!app->has_window || !app->fb || app->fb_w <= 0 || app->fb_h <= 0) return;    // 帧缓冲尺寸必须等于窗口尺寸（1:1 上屏）。对不上就重建。
    if (app->fb_w != app->width || app->fb_h != app->height) {
        rebuild_framebuffer(app);
        if (!app->fb || app->fb_w != app->width || app->fb_h != app->height) return;
    }

    Render r;
    render_init(&r, app->fb, app->fb_w, app->fb_h);
    if (!render_load_atlas(&r, app->atlas, app->atlas_len)) {
        LOGE("图集损坏，无法绘制");
        return;
    }
    render_load_font(&r, app->font, app->font_len);
    ui_paint(&r, &app->ui, &app->font_ctx, app->atlas, app->atlas_len, app->font, app->font_len);

    // 帧缓冲与窗口同尺寸，整屏一次贴出去即可（没有缩放层，也就没有留白要清）
    glViewport(0, 0, app->width, app->height);

    glUseProgram(app->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // 帧缓冲的内存顺序本来就是 R,G,B,A（CS_RGB(0xFF,0,0) == 0xFF0000FF，小端下
    // 落成 FF 00 00 FF），和 GL 要的 GL_RGBA 完全一致，所以**不做通道交换**。
    //
    // 之前这里用的是 GL_BGRA_EXT，那是错的：GLES2 规范里 glTexImage2D 的 format
    // **只保证接受 GL_RGBA**，GL_BGRA_EXT 需要 GL_EXT_texture_format_BGRA8888，
    // 属可选扩展。不支持的设备上这次调用会报错、纹理没被分配，采样返回 (0,0,0,1)，
    // 屏幕上就是一个刚好等于内容区域的黑矩形（真机实测就是这样）。
    //
    // 唯一要处理的是 alpha：帧缓冲里的 alpha 是贴图预乘合成的残留值，统一置 1，
    // 否则混合模式下会画出半透明。就地改 app->fb 即可，省一次整屏拷贝。
    cs_frame_opaque(app->fb, (size_t)app->fb_w * (size_t)app->fb_h);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, app->fb_w, app->fb_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, app->fb);
    {
        // 上传失败是这个应用最容易出的"全黑"故障，一定要留痕，别让它静默
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOGE("纹理上传失败 glTexImage2D error=0x%04x（%dx%d）", err, app->fb_w, app->fb_h);
        }
    }

    if (app->gl_ok) {
        glEnableVertexAttribArray((GLuint)app->a_pos);
        glEnableVertexAttribArray((GLuint)app->a_uv);
        glVertexAttribPointer((GLuint)app->a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), QUAD_VERTICES);
        glVertexAttribPointer((GLuint)app->a_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), QUAD_VERTICES + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray((GLuint)app->a_pos);
        glDisableVertexAttribArray((GLuint)app->a_uv);
        if (!app->logged_first_frame) {
            app->logged_first_frame = true;
            LOGI("首帧已绘制：GL_RENDERER=%s", (const char *)glGetString(GL_RENDERER));
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    if (!eglSwapBuffers(app->display, app->surface)) {
        LOGE("eglSwapBuffers 失败: 0x%04x", eglGetError());
    }
}

// ---------------------------------------------------------------- EGL
static bool egl_init(App *app) {
    app->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (app->display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay 失败");
        return false;
    }
    if (!eglInitialize(app->display, NULL, NULL)) {
        LOGE("eglInitialize 失败: 0x%04x", eglGetError());
        return false;
    }
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint num = 0;
    if (!eglChooseConfig(app->display, attrs, &config, 1, &num) || num < 1) {
        LOGE("eglChooseConfig 没找到可用配置");
        return false;
    }
    app->surface = eglCreateWindowSurface(app->display, config, app->app->window, NULL);
    if (app->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface 失败: 0x%04x", eglGetError());
        return false;
    }
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    app->context = eglCreateContext(app->display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (app->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext 失败: 0x%04x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(app->display, app->surface, app->surface, app->context)) {
        LOGE("eglMakeCurrent 失败: 0x%04x", eglGetError());
        return false;
    }
    EGLint w = 0, h = 0;
    eglQuerySurface(app->display, app->surface, EGL_WIDTH, &w);
    eglQuerySurface(app->display, app->surface, EGL_HEIGHT, &h);
    app->width = w > 0 ? w : ANativeWindow_getWidth(app->app->window);
    app->height = h > 0 ? h : ANativeWindow_getHeight(app->app->window);
    LOGI("EGL 就绪 %dx%d", app->width, app->height);
    return gl_setup(app);
}

static void egl_teardown(App *app) {
    if (app->display == EGL_NO_DISPLAY) return;
    gl_teardown(app);
    eglMakeCurrent(app->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (app->context != EGL_NO_CONTEXT) eglDestroyContext(app->display, app->context);
    if (app->surface != EGL_NO_SURFACE) eglDestroySurface(app->display, app->surface);
    eglTerminate(app->display);
    app->context = EGL_NO_CONTEXT;
    app->surface = EGL_NO_SURFACE;
    app->display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------- 单格尺寸
// 目标 0.7cm。1 英寸 = 25.4mm，所以 0.7cm 的像素数 = 7.0 * dpi / 25.4。
//
// 密度取 **getResources().getDisplayMetrics().densityDpi**：
// 这是安卓自己"这台设备当前真正在用"的密度，能反映 `wm density` 覆盖、
// 厂商自定义值等。相比之下 AConfiguration_getDensity 只给"桶值"（160/240/320…），
// 420/440 这种真实密度会被抹平。
//
// **这里踩过一个很关键的坑**：android_main 跑在 native 线程上，那个线程
// 默认**没有 attach 到 JVM**，所以 `GetEnv` 返回 JNI_EDETACHED(-2)，
// 整个查询直接失败、退回兜底常量 280 —— 表现是"无论屏幕密度是多少，
// 单格都按 280dpi 算"。在 440dpi 的真机上格子会只有 0.44cm（该是 0.70cm），
// 手机上就会明显偏小。必须先 AttachCurrentThread。
#define CS_FALLBACK_DENSITY 280

static int32_t query_density_dpi(struct android_app *android_app) {
    JNIEnv *env = NULL;
    JavaVM *vm = android_app->activity->vm;
    if (!vm) return CS_FALLBACK_DENSITY;

    // 关键：native 线程通常还没 attach，GetEnv 会返回 JNI_EDETACHED。
    // 只 attach 一次（attached 标志），进程存活期间不 detach。
    static bool attached = false;
    jint ge = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (ge == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) {
            LOGE("密度查询：AttachCurrentThread 失败，退回 %d dpi", CS_FALLBACK_DENSITY);
            return CS_FALLBACK_DENSITY;
        }
        attached = true;
    } else if (ge != JNI_OK) {
        LOGE("密度查询：GetEnv 失败=%d，退回 %d dpi", (int)ge, CS_FALLBACK_DENSITY);
        return CS_FALLBACK_DENSITY;
    }
    (void)attached;

    jobject activity = android_app->activity->clazz;
    if (!activity) return CS_FALLBACK_DENSITY;
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) return CS_FALLBACK_DENSITY;

    int32_t dpi = 0;
    jmethodID get_res = (*env)->GetMethodID(env, cls, "getResources", "()Landroid/content/res/Resources;");
    if (get_res) {
        jobject res = (*env)->CallObjectMethod(env, activity, get_res);
        if (res) {
            jclass rcls = (*env)->GetObjectClass(env, res);
            // 路径 A（首选）：getDisplayMetrics().densityDpi —— 真实值
            jmethodID get_dm = (*env)->GetMethodID(env, rcls, "getDisplayMetrics",
                                                   "()Landroid/util/DisplayMetrics;");
            if (get_dm) {
                jobject dm = (*env)->CallObjectMethod(env, res, get_dm);
                if (dm) {
                    jclass dcls = (*env)->GetObjectClass(env, dm);
                    jfieldID f = (*env)->GetFieldID(env, dcls, "densityDpi", "I");
                    if (f) {
                        const jint v = (*env)->GetIntField(env, dm, f);
                        if (v > 0) dpi = (int32_t)v;
                    }
                    (*env)->DeleteLocalRef(env, dcls);
                    (*env)->DeleteLocalRef(env, dm);
                }
            }
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            // 路径 B（兜底）：getConfiguration().densityDpi
            if (dpi <= 0) {
                jmethodID get_cfg = (*env)->GetMethodID(env, rcls, "getConfiguration",
                                                        "()Landroid/content/res/Configuration;");
                if (get_cfg) {
                    jobject cfg = (*env)->CallObjectMethod(env, res, get_cfg);
                    if (cfg) {
                        jclass ccls = (*env)->GetObjectClass(env, cfg);
                        jfieldID f = (*env)->GetFieldID(env, ccls, "densityDpi", "I");
                        if (f) {
                            const jint v = (*env)->GetIntField(env, cfg, f);
                            if (v > 0) dpi = (int32_t)v;
                        }
                        (*env)->DeleteLocalRef(env, ccls);
                        (*env)->DeleteLocalRef(env, cfg);
                    }
                }
            }
            (*env)->DeleteLocalRef(env, rcls);
            (*env)->DeleteLocalRef(env, res);
        }
    }
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (dpi <= 0) {
        LOGW("密度查询失败（两条路径都没取到），退回 %d dpi", CS_FALLBACK_DENSITY);
        return CS_FALLBACK_DENSITY;
    }
    return dpi;
}

// 目标单格边长（0.7cm）在给定密度下是多少设备像素。
// 换算本身放在 ui_cell_px_for_dpi（界面层），这样宿主机自检能把它钉死；
// 这里只是设备侧的入口。
static int32_t cell_px_for_density(int32_t dpi) {
    return ui_cell_px_for_dpi(dpi);
}

// ---------------------------------------------------------------- 触摸
// 手势语义在 ui.c 里（单点点按插旗 / 长按翻开 / 双击展开 / 拖动棋盘），
// 这里只负责把安卓的 AMotionEvent 翻译成 ui_pointer_* 调用。
static int32_t handle_input(App *app, AInputEvent *event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const size_t idx = (size_t)((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const int32_t pid = AMotionEvent_getPointerId(event, idx);
    const float fx = AMotionEvent_getX(event, idx);
    const float fy = AMotionEvent_getY(event, idx);
    int32_t ux = 0, uy = 0;
    screen_to_ui(app, (int32_t)fx, (int32_t)fy, &ux, &uy);
    app->now_ms = tick_ms();

    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            // 第二根手指按下：忽略（本项目没有多指手势；忽略比当成"新的单指"安全，
            // 免得两指乱按导致误插旗）
            if (app->touch_active) return 1;
            app->touch_active = true;
            app->touch_id = pid;
            app->touch_x = ux;
            app->touch_y = uy;
            app->touch_down_ms = app->now_ms;
            ui_pointer_down(&app->ui, ux, uy, app->now_ms);
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            if (!app->touch_active || pid != app->touch_id) return 1;
            if (ux == app->touch_x && uy == app->touch_y) return 1;
            app->touch_x = ux;
            app->touch_y = uy;
            ui_pointer_move(&app->ui, ux, uy, app->now_ms);
            // 平移是"一有位移就发生"的（没有死区），所以这里必须立刻请求出帧；
            // 拖动/点按的最终判定要到松手时才做（见 ui_pointer_up）。
            app->need_frame = true;
            return 1;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (!app->touch_active) return 1;
            if (pid != app->touch_id) return 1;
            app->touch_active = false;
            ui_pointer_up(&app->ui, ux, uy, app->now_ms);
            // 松手时若判定为"点按"，ui_pointer_up 内部会把偏移恢复成按下时的值；
            // 那一帧必须重画，否则屏幕上会留下"抖动后的画面"。
            app->need_frame = true;
            return 1;
        case AMOTION_EVENT_ACTION_CANCEL:
            if (app->touch_active) {
                app->touch_active = false;
                // 系统取消（来电/手势冲突）：拖动也一样收尾，别把按格手势留在半路
                if (app->ui.pan_active) {
                    ui_pan_end(&app->ui);
                    ui_pointer_cancel(&app->ui);
                }
                else {
                    ui_pointer_up(&app->ui, app->touch_x, app->touch_y, app->now_ms);
                }
            }
            return 1;
        default:
            return 1;
    }
}

// ---------------------------------------------------------------- 生命周期
// 这里有一条很重要的分工（踩过大坑）：
//   - `has_window` 才代表"能不能画"（EGL 与帧缓冲是否就绪）
//   - `need_frame` 代表"有一帧欠着没画"
//   - `animating` 只代表"界面自己在动"（按着不放、等双击窗口、脸在闪）
// 早先用 `APP_CMD_LOST_FOCUS` 把 animating 置 false 当总开关，后果是：
// 模拟器里 LOST_FOCUS 紧跟在 INIT_WINDOW 后面到达，主循环从此用
// `ALooper_pollOnce(-1)` 永久阻塞。之后每次触摸虽然都被 native_app_glue 在**主线程**
// 处理好（日志里能看到），但那一步发生在 timeout 已经取成 -1 之后 —— 内层 while
// 处理完输入立刻又阻塞，永远走不到绘制代码。玩家看到的就是"点了完全没反应"。
// 现在焦点不再参与是否绘制，输入一律会置 need_frame，主循环也用有界超时兜底。
static void on_app_cmd(struct android_app *android_app, int32_t cmd) {
    App *app = (App *)android_app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            app->has_window = android_app->window != NULL;
            if (app->has_window && app->display == EGL_NO_DISPLAY) {
                if (!egl_init(app)) {
                    LOGE("EGL 初始化失败，画面出不来");
                    app->has_window = false;
                    return;
                }
            }
            if (app->has_window) {
                app->width = ANativeWindow_getWidth(android_app->window);
                app->height = ANativeWindow_getHeight(android_app->window);
                rebuild_framebuffer(app);
                app->need_frame = true;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            app->has_window = false;
            app->animating = false;
            app->need_frame = false;
            egl_teardown(app);
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (app->has_window) {
                app->width = ANativeWindow_getWidth(android_app->window);
                app->height = ANativeWindow_getHeight(android_app->window);
                rebuild_framebuffer(app);
                app->need_frame = true;
            }
            break;
        case APP_CMD_GAINED_FOCUS:
            app->focused = true;
            // 回到前台：把"暂停期间流逝的时间"从计时里补掉，
            // 否则锁屏一会儿回来计时器就飞了（成绩也就没意义了）。
            if (app->paused_at_ms != 0 && app->ui.game.t0 != 0) {
                app->ui.game.t0 += tick_ms() - app->paused_at_ms;
            }
            app->paused_at_ms = 0;
            app->need_frame = true;
            break;
        case APP_CMD_LOST_FOCUS:
            app->focused = false;
            // 记下失焦时刻，回前台时用它把这段时间从计时里扣掉。
            // 注意：**不**停绘制 —— 焦点与"能不能画"无关（见函数头注释）。
            app->paused_at_ms = tick_ms();
            break;
        default:
            break;
    }
}

// onInputEvent 的签名要求返回 int32_t（1 = 已消费，0 = 交回系统）
static int32_t on_input(struct android_app *android_app, AInputEvent *event) {
    App *app = (App *)android_app->userData;
    if (!app->has_window) return 0;
    const int32_t handled = handle_input(app, event);
    // 只要碰过输入就欠一帧：这是"触摸后画面必须立刻变"的保证
    app->need_frame = true;
    return handled;
}

void android_main(struct android_app *android_app) {
    static App app;
    memset(&app, 0, sizeof app);
    app.app = android_app;
    app.display = EGL_NO_DISPLAY;
    app.surface = EGL_NO_SURFACE;
    app.context = EGL_NO_CONTEXT;
    app.touch_id = -1;

    android_app->onAppCmd = on_app_cmd;
    android_app->onInputEvent = on_input;
    android_app->userData = &app;

    // 图集与字形图集：都从 assets 读。读不到就直接报错退出，
    // 省得拿着空指针一路画下去、只看到一片灰屏还找不到原因。
    AAssetManager *mgr = android_app->activity->assetManager;
    if (!read_asset(mgr, "atlas.bin", &app.atlas, &app.atlas_len)) {
        LOGE("assets/atlas.bin 读取失败，退出");
        return;
    }
    if (!read_asset(mgr, "font_atlas.bin", &app.font, &app.font_len)) {
        LOGE("assets/font_atlas.bin 读取失败，退出");
        return;
    }

    Render probe;
    render_init(&probe, NULL, 0, 0);
    if (!render_load_atlas(&probe, app.atlas, app.atlas_len)) {
        LOGE("图集格式不认识，退出");
        return;
    }
    render_load_font(&probe, app.font, app.font_len);
    if (!font_init(&app.font_ctx, &probe)) {
        LOGE("字形图集格式不认识，退出");
        return;
    }

    // 先用一个保守的默认值初始化界面；真实尺寸在下面的 APP_CMD_INIT_WINDOW 里
    // 由 rebuild_framebuffer → ui_resize 覆盖（这两步必须成对，缺了就会在
    // 非 1920x1080 的设备上画错 —— 曾经漏过，见 rebuild_framebuffer 的说明）。
    ui_init(&app.ui, 1920, 1080);

    // 单格边长按物理尺寸定：0.7cm × 屏幕密度。密度在这里查一次就够，
    // 它不会随旋转变化（变了也只是格子大小跟着变，不影响正确性）。
    const int32_t dpi = query_density_dpi(android_app);
    app.cell_px = cell_px_for_density(dpi);
    ui_set_cell_px(&app.ui, app.cell_px);
    LOGI("复扫雷安卓版启动（%s）", CS_APP_VERSION_STR);
    // 像素 → 厘米：px × 25.4 / dpi 得到的是**毫米**，再 /10 才是厘米
    LOGI("屏幕密度 %d dpi → 单格 %dpx（= %.2fcm，目标 0.70cm）", dpi, app.cell_px,
         app.cell_px * 25.4 / dpi / 10.0);

    app.now_ms = tick_ms();
    app.need_frame = true;
    for (;;) {
        struct android_poll_source *source = NULL;

        // ---- 出帧策略：一次循环 = 取一次事件 + 务必出一帧 ----
        //
        // **这里不能写成 while 循环**（踩过一个大坑，症状就是"拖动不平滑"）：
        //   while (ALooper_pollOnce(timeout, ...) >= 0) { source->process(...); }
        // 拖动时输入队列的 fd **一直是可读的**（手指持续产生 MOVE），
        // 于是这个 while 永远有活干、**永远不退出**，后面的绘制代码一次都轮不到。
        // 表现就是：只有事件流出现空隙（手指停住、松手）时才出帧 ——
        // 手感上正是"必须先按住一小段时间才能平滑拖动；不等一下就一顿一顿"。
        //
        // 现在改成每轮循环**只取一次事件**：取完就往下走去绘制，绘制完再回来取下一个。
        // 事件因此变成"每帧最多处理一次"，但输入本身是排队的，不会丢；
        // 而且绘制拿到了与 vsync 对齐的稳定节奏（实测 16~17ms/帧）。
        //
        // 超时策略：
        //   - 界面自己在动（按着 / 等双击窗口 / 脸在闪）→ 0，立刻回来看一帧
        //   - 空闲 → 50ms 有界轮询（**不能写 -1**：输入在主线程的 source->process()
        //     里处理，那一步发生在 timeout 取成 -1 之后，处理完会立刻再阻塞，
        //     于是"输入处理了但画面永不更新"—— 这是最早"触屏完全没反应"的根因）
        const bool busy = app.animating || app.ui.pointer_down || app.ui.tap_armed;
        const int timeout = busy ? 0 : 50;
        {
            // 只关心出参 source（谁要处理事件），事件位掩码本身用不上
            int poll_events = 0;
            struct android_poll_source *poll_source = NULL;
            if (ALooper_pollOnce(timeout, NULL, &poll_events, (void **)&poll_source) >= 0) {
                source = poll_source;
            }
            if (source) source->process(android_app, source);
            if (android_app->destroyRequested) {
                egl_teardown(&app);
                free(app.fb);
                free(app.atlas);
                free(app.font);
                return;
            }
        }
        if (!app.has_window || !app.gl_ok) continue;

        app.now_ms = tick_ms();
        // 手势里的"等双击窗口到期"要靠 tick 推进，所以每次循环都要调。
        // 面板计时用的是"now - t0"，不是累加，所以多画几帧不会让计时变快。
        ui_tick(&app.ui, app.now_ms);
        draw_frame(&app);
        app.need_frame = false;
        // 还有欠账就继续连续出帧：按着不放、点按待定、脸在闪，都算
        app.animating = app.ui.pointer_down || app.ui.tap_armed ||
                        app.now_ms < app.ui.face_flash_until;
    }
}
