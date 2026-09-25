// Platform for standalone OpenXR headsets running Android (Meta Quest). Uses
// NativeActivity, EGL/GLES3, OpenXR and AAudio. Rendering is done by
// render_vr.c: races are drawn in stereo with the HUD on a quad layer; all
// other scenes (intro, title, menus) are shown on a virtual screen.

#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "platform.h"
#include "input.h"
#include "system.h"
#include "render.h"
#include "render_vr.h"
#include "utils.h"
#include "mem.h"

#include "wipeout/game.h"

#define LOG_TAG "wipeout"
#define log_info(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Size of the virtual screen / HUD texture and its placement in meters,
// relative to the head position at startup (or the last recenter).
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080
#define SCREEN_DISTANCE 2.5f
#define SCREEN_WORLD_WIDTH 3.2f
#define SCREEN_WORLD_HEIGHT (SCREEN_WORLD_WIDTH * SCREEN_HEIGHT / SCREEN_WIDTH)

#define EYE_MSAA_SAMPLES 4
#define PREFERRED_REFRESH_RATE 90.0f

#define xr_check(RES, ...) \
	do { \
		XrResult xr_check_res = (RES); \
		if (XR_FAILED(xr_check_res)) { \
			printf("OpenXR error %d\n", xr_check_res); \
			die(__VA_ARGS__); \
		} \
	} while (0)


static struct android_app *android_app;
static bool app_resumed = false;
static bool wants_to_exit = false;
static char *path_assets = "";
static char *path_userdata = "";
static char *temp_path = NULL;

static void (*audio_callback)(float *buffer, uint32_t len) = NULL;
static AAudioStream *audio_stream = NULL;

static EGLDisplay egl_display;
static EGLConfig egl_config;
static EGLContext egl_context;
static EGLSurface egl_surface;

static PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT_ = NULL;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC glRenderbufferStorageMultisampleEXT_ = NULL;

typedef struct {
	XrSwapchain handle;
	vec2i_t size;
	uint32_t images_len;
	GLuint *fbos;
	GLuint depth_buffer;
} swapchain_t;

static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system = XR_NULL_SYSTEM_ID;
static XrSession xr_session = XR_NULL_HANDLE;
static XrSpace xr_local_space = XR_NULL_HANDLE;
static XrSessionState xr_session_state = XR_SESSION_STATE_UNKNOWN;
static bool xr_session_running = false;
static bool xr_has_refresh_rate = false;
static bool xr_has_bd_controllers = false;
static XrViewConfigurationView xr_config_views[2];
static swapchain_t eye_swapchains[2];
static swapchain_t screen_swapchain;



// -----------------------------------------------------------------------------
// Logging: forward stdout/stderr to logcat, so the game's printf()s show up

static int log_pipe[2];

static void *log_thread(void *arg) {
	char buf[512];
	ssize_t len;
	while ((len = read(log_pipe[0], buf, sizeof(buf) - 1)) > 0) {
		if (buf[len - 1] == '\n') {
			len--;
		}
		buf[len] = 0;
		__android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
	}
	return NULL;
}

static void log_flush_at_exit(void) {
	// die() calls exit() right after printing; give the log thread a moment
	fflush(stdout);
	fflush(stderr);
	usleep(200 * 1000);
}

static void log_redirect_stdout(void) {
	setvbuf(stdout, 0, _IOLBF, 0);
	setvbuf(stderr, 0, _IONBF, 0);
	pipe(log_pipe);
	dup2(log_pipe[1], STDOUT_FILENO);
	dup2(log_pipe[1], STDERR_FILENO);
	pthread_t thread;
	pthread_create(&thread, NULL, log_thread, NULL);
	pthread_detach(thread);
	atexit(log_flush_at_exit);
}



// -----------------------------------------------------------------------------
// Platform API

void platform_exit(void) {
	wants_to_exit = true;
}

vec2i_t platform_screen_size(void) {
	return vec2i(SCREEN_WIDTH, SCREEN_HEIGHT);
}

double platform_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

bool platform_get_fullscreen(void) {
	return true;
}

void platform_set_fullscreen(bool fullscreen) {
	// Always fullscreen
}

static aaudio_data_callback_result_t platform_audio_callback(AAudioStream *stream, void *user, void *data, int32_t frames) {
	if (audio_callback) {
		audio_callback((float *)data, frames * 2);
	}
	else {
		memset(data, 0, frames * 2 * sizeof(float));
	}
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void platform_audio_open(void) {
	AAudioStreamBuilder *builder;
	if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
		printf("AAudio_createStreamBuilder failed\n");
		return;
	}
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setChannelCount(builder, 2);
	AAudioStreamBuilder_setSampleRate(builder, 44100);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setDataCallback(builder, platform_audio_callback, NULL);
	aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &audio_stream);
	AAudioStreamBuilder_delete(builder);
	if (res != AAUDIO_OK) {
		printf("AAudio openStream failed: %s\n", AAudio_convertResultToText(res));
		audio_stream = NULL;
	}
}

static void platform_audio_set_playing(bool playing) {
	if (!audio_stream) {
		return;
	}
	if (playing && audio_callback) {
		AAudioStream_requestStart(audio_stream);
	}
	else {
		AAudioStream_requestPause(audio_stream);
	}
}

void platform_set_audio_mix_cb(void (*cb)(float *buffer, uint32_t len)) {
	audio_callback = cb;
	platform_audio_set_playing(app_resumed);
}

bool platform_asset_exists(const char *name) {
	char *path = strcat(strcpy(temp_path, path_assets), name);
	return file_exists(path);
}

FILE *platform_open_asset(const char *name, const char *mode) {
	char *path = strcat(strcpy(temp_path, path_assets), name);
	return fopen(path, mode);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
	char *path = strcat(strcpy(temp_path, path_assets), name);
	return file_load(path, bytes_read);
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	char *path = strcat(strcpy(temp_path, path_userdata), name);
	if (!file_exists(path)) {
		*bytes_read = 0;
		return NULL;
	}
	return file_load(path, bytes_read);
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	char *path = strcat(strcpy(temp_path, path_userdata), name);
	return file_store(path, bytes, len);
}



// -----------------------------------------------------------------------------
// Game data import. Directories created through adb in the app's external
// files dir belong to the shell user and can't be read by the app. So the game
// data is pushed as a single tar file (see android/push_data.sh) and unpacked
// here on startup.

// Create all directories leading up to the last '/' in path
static void mkdir_parents(char *path) {
	for (char *p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0770);
			*p = '/';
		}
	}
}

static void platform_import_data_tar(void) {
	char *tar_path = strcat(strcpy(temp_path, path_assets), "wipeout.tar");
	FILE *f = fopen(tar_path, "rb");
	if (!f) {
		return;
	}
	printf("importing %s\n", tar_path);

	char *out_path = malloc(strlen(path_assets) + 256 + 2);
	uint8_t *buffer = malloc(64 * 1024);
	uint8_t header[512];
	uint32_t files_len = 0;
	bool failed = false;

	while (fread(header, 1, sizeof(header), f) == sizeof(header) && header[0]) {
		// ustar: optional prefix + name
		char name[256 + 2] = {0};
		if (header[345]) {
			snprintf(name, sizeof(name), "%.155s/%.100s", header + 345, header);
		}
		else {
			snprintf(name, sizeof(name), "%.100s", header);
		}
		char size_str[13] = {0};
		memcpy(size_str, header + 124, 12);
		uint32_t size = strtoul(size_str, NULL, 8);
		char type = header[156];

		// Only accept plain relative paths inside the files dir
		if (name[0] == '/' || strstr(name, "..")) {
			printf("skipping invalid tar entry %s\n", name);
			failed = true;
			break;
		}

		strcpy(out_path, path_assets);
		strcat(out_path, name);

		if (type == '5') {
			strcat(out_path, "/");
			mkdir_parents(out_path);
		}
		else if (type == '0' || type == 0) {
			mkdir_parents(out_path);
			FILE *out = fopen(out_path, "wb");
			if (!out) {
				printf("failed to write %s\n", out_path);
				failed = true;
				break;
			}
			uint32_t remaining = size;
			while (remaining) {
				uint32_t chunk = min(remaining, 64 * 1024);
				if (fread(buffer, 1, chunk, f) != chunk) {
					failed = true;
					break;
				}
				fwrite(buffer, 1, chunk, out);
				remaining -= chunk;
			}
			fclose(out);
			if (failed) {
				break;
			}
			files_len++;
		}
		else {
			// Skip the data of other entry types
			fseek(f, size, SEEK_CUR);
		}

		// Entries are padded to 512 bytes
		fseek(f, (512 - (size % 512)) % 512, SEEK_CUR);
	}

	fclose(f);
	free(buffer);
	free(out_path);

	if (failed) {
		printf("import failed after %d files; keeping %s\n", files_len, tar_path);
	}
	else {
		printf("imported %d files\n", files_len);
		remove(strcat(strcpy(temp_path, path_assets), "wipeout.tar"));
	}
}



// -----------------------------------------------------------------------------
// EGL

static void platform_egl_init(void) {
	egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(egl_display, NULL, NULL);

	EGLint config_attribs[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_NONE
	};
	EGLint num_configs = 0;
	eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs);
	error_if(num_configs == 0, "No suitable EGL config");

	EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
	error_if(egl_context == EGL_NO_CONTEXT, "eglCreateContext failed");

	// The context needs a surface to be made current; everything is drawn
	// into OpenXR swapchains though.
	EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	egl_surface = eglCreatePbufferSurface(egl_display, egl_config, surface_attribs);
	eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (strstr(extensions, "GL_EXT_multisampled_render_to_texture")) {
		glFramebufferTexture2DMultisampleEXT_ = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)
			eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
		glRenderbufferStorageMultisampleEXT_ = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
			eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	}
}

static void platform_egl_cleanup(void) {
	eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(egl_display, egl_surface);
	eglDestroyContext(egl_display, egl_context);
	eglTerminate(egl_display);
}



// -----------------------------------------------------------------------------
// OpenXR setup

static bool xr_has_extension(XrExtensionProperties *props, uint32_t len, const char *name) {
	for (uint32_t i = 0; i < len; i++) {
		if (strcmp(props[i].extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static void platform_xr_create_instance(void) {
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
	error_if(!xrInitializeLoaderKHR, "xrInitializeLoaderKHR not found");

	XrLoaderInitInfoAndroidKHR loader_info = {
		.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
		.applicationVM = android_app->activity->vm,
		.applicationContext = android_app->activity->clazz,
	};
	xr_check(xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR *)&loader_info), "xrInitializeLoaderKHR");

	uint32_t ext_len = 0;
	xr_check(xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_len, NULL), "enumerate extensions");
	XrExtensionProperties *ext_props = mem_temp_alloc(sizeof(XrExtensionProperties) * ext_len);
	for (uint32_t i = 0; i < ext_len; i++) {
		ext_props[i] = (XrExtensionProperties){.type = XR_TYPE_EXTENSION_PROPERTIES};
	}
	xr_check(xrEnumerateInstanceExtensionProperties(NULL, ext_len, &ext_len, ext_props), "enumerate extensions");

	const char *extensions[8];
	uint32_t extensions_len = 0;
	extensions[extensions_len++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	extensions[extensions_len++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	error_if(!xr_has_extension(ext_props, ext_len, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME), "OpenXR runtime has no GLES support");
	if (xr_has_extension(ext_props, ext_len, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
		extensions[extensions_len++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
		xr_has_refresh_rate = true;
	}
	if (xr_has_extension(ext_props, ext_len, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
		extensions[extensions_len++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
		xr_has_bd_controllers = true;
	}
	mem_temp_free(ext_props);

	XrInstanceCreateInfoAndroidKHR android_info = {
		.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
		.applicationVM = android_app->activity->vm,
		.applicationActivity = android_app->activity->clazz,
	};
	XrInstanceCreateInfo create_info = {
		.type = XR_TYPE_INSTANCE_CREATE_INFO,
		.next = &android_info,
		.applicationInfo = {
			.applicationName = "wipEout",
			.applicationVersion = 1,
			.engineName = "wipeout-rewrite",
			.engineVersion = 1,
			.apiVersion = XR_API_VERSION_1_0,
		},
		.enabledExtensionCount = extensions_len,
		.enabledExtensionNames = extensions,
	};
	xr_check(xrCreateInstance(&create_info, &xr_instance), "xrCreateInstance");

	XrSystemGetInfo system_info = {
		.type = XR_TYPE_SYSTEM_GET_INFO,
		.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
	};
	xr_check(xrGetSystem(xr_instance, &system_info, &xr_system), "xrGetSystem");
}

static void platform_xr_create_session(void) {
	// Required to be called before creating the session
	PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR;
	xrGetInstanceProcAddr(xr_instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&xrGetOpenGLESGraphicsRequirementsKHR);
	XrGraphicsRequirementsOpenGLESKHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	xr_check(xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &requirements), "graphics requirements");

	XrGraphicsBindingOpenGLESAndroidKHR binding = {
		.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
		.display = egl_display,
		.config = egl_config,
		.context = egl_context,
	};
	XrSessionCreateInfo session_info = {
		.type = XR_TYPE_SESSION_CREATE_INFO,
		.next = &binding,
		.systemId = xr_system,
	};
	xr_check(xrCreateSession(xr_instance, &session_info, &xr_session), "xrCreateSession");

	XrReferenceSpaceCreateInfo space_info = {
		.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
		.poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	xr_check(xrCreateReferenceSpace(xr_session, &space_info, &xr_local_space), "xrCreateReferenceSpace");
}

static int64_t platform_xr_choose_format(void) {
	uint32_t formats_len = 0;
	xr_check(xrEnumerateSwapchainFormats(xr_session, 0, &formats_len, NULL), "enumerate formats");
	int64_t *formats = mem_temp_alloc(sizeof(int64_t) * formats_len);
	xr_check(xrEnumerateSwapchainFormats(xr_session, formats_len, &formats_len, formats), "enumerate formats");

	// The game outputs sRGB encoded colors. Prefer an sRGB swapchain, so the
	// compositor knows that, but only if we can switch off the conversion on
	// write.
	bool has_srgb_write_control = strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_sRGB_write_control") != NULL;
	int64_t format = 0;
	for (uint32_t i = 0; i < formats_len; i++) {
		if (formats[i] == GL_SRGB8_ALPHA8 && has_srgb_write_control) {
			format = formats[i];
			break;
		}
		if (formats[i] == GL_RGBA8 && !format) {
			format = formats[i];
		}
	}
	mem_temp_free(formats);
	error_if(!format, "No suitable swapchain format");

	if (format == GL_SRGB8_ALPHA8) {
		glDisable(GL_FRAMEBUFFER_SRGB_EXT);
	}
	return format;
}

static swapchain_t platform_xr_create_swapchain(int64_t format, vec2i_t size, bool with_depth) {
	swapchain_t sc = {.size = size};
	XrSwapchainCreateInfo info = {
		.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		.format = format,
		.sampleCount = 1,
		.width = size.x,
		.height = size.y,
		.faceCount = 1,
		.arraySize = 1,
		.mipCount = 1,
	};
	xr_check(xrCreateSwapchain(xr_session, &info, &sc.handle), "xrCreateSwapchain");

	xr_check(xrEnumerateSwapchainImages(sc.handle, 0, &sc.images_len, NULL), "enumerate images");
	XrSwapchainImageOpenGLESKHR *images = mem_temp_alloc(sizeof(XrSwapchainImageOpenGLESKHR) * sc.images_len);
	for (uint32_t i = 0; i < sc.images_len; i++) {
		images[i] = (XrSwapchainImageOpenGLESKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
	}
	xr_check(xrEnumerateSwapchainImages(sc.handle, sc.images_len, &sc.images_len, (XrSwapchainImageBaseHeader *)images), "enumerate images");

	bool msaa = with_depth && glFramebufferTexture2DMultisampleEXT_ && glRenderbufferStorageMultisampleEXT_;
	if (with_depth) {
		glGenRenderbuffers(1, &sc.depth_buffer);
		glBindRenderbuffer(GL_RENDERBUFFER, sc.depth_buffer);
		if (msaa) {
			glRenderbufferStorageMultisampleEXT_(GL_RENDERBUFFER, EYE_MSAA_SAMPLES, GL_DEPTH_COMPONENT24, size.x, size.y);
		}
		else {
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
		}
	}

	sc.fbos = mem_bump(sizeof(GLuint) * sc.images_len);
	glGenFramebuffers(sc.images_len, sc.fbos);
	for (uint32_t i = 0; i < sc.images_len; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
		if (msaa) {
			glFramebufferTexture2DMultisampleEXT_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i].image, 0, EYE_MSAA_SAMPLES);
		}
		else {
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i].image, 0);
		}
		if (with_depth) {
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth_buffer);
		}
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		error_if(status != GL_FRAMEBUFFER_COMPLETE, "Incomplete swapchain framebuffer 0x%x", status);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	mem_temp_free(images);

	return sc;
}

static void platform_xr_create_swapchains(void) {
	uint32_t views_len = 0;
	xr_check(xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &views_len, NULL), "enumerate views");
	error_if(views_len != 2, "Expected 2 views, got %d", views_len);
	for (int i = 0; i < 2; i++) {
		xr_config_views[i] = (XrViewConfigurationView){.type = XR_TYPE_VIEW_CONFIGURATION_VIEW};
	}
	xr_check(xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views_len, xr_config_views), "enumerate views");

	int64_t format = platform_xr_choose_format();
	for (int i = 0; i < 2; i++) {
		vec2i_t size = vec2i(xr_config_views[i].recommendedImageRectWidth, xr_config_views[i].recommendedImageRectHeight);
		eye_swapchains[i] = platform_xr_create_swapchain(format, size, true);
		printf("eye %d swapchain %dx%d\n", i, size.x, size.y);
	}
	screen_swapchain = platform_xr_create_swapchain(format, vec2i(SCREEN_WIDTH, SCREEN_HEIGHT), false);
}

static void platform_xr_set_refresh_rate(void) {
	if (!xr_has_refresh_rate) {
		return;
	}
	PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB;
	PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB;
	xrGetInstanceProcAddr(xr_instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&xrEnumerateDisplayRefreshRatesFB);
	xrGetInstanceProcAddr(xr_instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&xrRequestDisplayRefreshRateFB);

	float rates[16];
	uint32_t rates_len = 0;
	if (XR_FAILED(xrEnumerateDisplayRefreshRatesFB(xr_session, len(rates), &rates_len, rates))) {
		return;
	}
	for (uint32_t i = 0; i < rates_len; i++) {
		if (rates[i] == PREFERRED_REFRESH_RATE) {
			xrRequestDisplayRefreshRateFB(xr_session, rates[i]);
			printf("refresh rate %.0f Hz\n", rates[i]);
			return;
		}
	}
}



// -----------------------------------------------------------------------------
// Input: Touch controllers are mapped onto the gamepad buttons

typedef enum {
	ACTION_BOOL,
	ACTION_FLOAT,
	ACTION_STICK,
} action_kind_t;

typedef struct {
	const char *name;
	action_kind_t kind;
	const char *path;
	button_t button; // for sticks: the UP button; DOWN, LEFT, RIGHT follow
	XrAction action;
} quest_action_t;

static quest_action_t quest_actions[] = {
	{"button_a",      ACTION_BOOL,  "/user/hand/right/input/a/click",          INPUT_GAMEPAD_A},
	{"button_b",      ACTION_BOOL,  "/user/hand/right/input/b/click",          INPUT_GAMEPAD_B},
	{"button_x",      ACTION_BOOL,  "/user/hand/left/input/x/click",           INPUT_GAMEPAD_X},
	{"button_y",      ACTION_BOOL,  "/user/hand/left/input/y/click",           INPUT_GAMEPAD_Y},
	{"button_menu",   ACTION_BOOL,  "/user/hand/left/input/menu/click",        INPUT_GAMEPAD_START},
	{"stick_click_l", ACTION_BOOL,  "/user/hand/left/input/thumbstick/click",  INPUT_GAMEPAD_L_STICK_PRESS},
	{"stick_click_r", ACTION_BOOL,  "/user/hand/right/input/thumbstick/click", INPUT_GAMEPAD_R_STICK_PRESS},
	{"trigger_l",     ACTION_FLOAT, "/user/hand/left/input/trigger/value",     INPUT_GAMEPAD_L_TRIGGER},
	{"trigger_r",     ACTION_FLOAT, "/user/hand/right/input/trigger/value",    INPUT_GAMEPAD_R_TRIGGER},
	{"squeeze_l",     ACTION_FLOAT, "/user/hand/left/input/squeeze/value",     INPUT_GAMEPAD_L_SHOULDER},
	{"squeeze_r",     ACTION_FLOAT, "/user/hand/right/input/squeeze/value",    INPUT_GAMEPAD_R_SHOULDER},
	{"stick_l",       ACTION_STICK, "/user/hand/left/input/thumbstick",        INPUT_GAMEPAD_L_STICK_UP},
	{"stick_r",       ACTION_STICK, "/user/hand/right/input/thumbstick",       INPUT_GAMEPAD_R_STICK_UP},
};

static XrActionSet xr_action_set;

static void platform_xr_create_actions(void) {
	XrActionSetCreateInfo set_info = {.type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = 0};
	strcpy(set_info.actionSetName, "gameplay");
	strcpy(set_info.localizedActionSetName, "Gameplay");
	xr_check(xrCreateActionSet(xr_instance, &set_info, &xr_action_set), "xrCreateActionSet");

	XrActionSuggestedBinding bindings[len(quest_actions)];
	for (int i = 0; i < len(quest_actions); i++) {
		quest_action_t *qa = &quest_actions[i];
		XrActionCreateInfo info = {.type = XR_TYPE_ACTION_CREATE_INFO};
		info.actionType =
			qa->kind == ACTION_BOOL ? XR_ACTION_TYPE_BOOLEAN_INPUT :
			qa->kind == ACTION_FLOAT ? XR_ACTION_TYPE_FLOAT_INPUT :
			XR_ACTION_TYPE_VECTOR2F_INPUT;
		strcpy(info.actionName, qa->name);
		strcpy(info.localizedActionName, qa->name);
		xr_check(xrCreateAction(xr_action_set, &info, &qa->action), "xrCreateAction %s", qa->name);

		bindings[i].action = qa->action;
		xr_check(xrStringToPath(xr_instance, qa->path, &bindings[i].binding), "xrStringToPath %s", qa->path);
	}

	// Meta Touch and Pico controllers have the same buttons under the same
	// paths. Profiles the runtime doesn't know are rejected; that's fine as
	// long as one of them is accepted.
	const char *profiles[] = {
		"/interaction_profiles/oculus/touch_controller",
		"/interaction_profiles/bytedance/pico4_controller",
		"/interaction_profiles/bytedance/pico4s_controller",
		"/interaction_profiles/bytedance/pico_neo3_controller",
	};
	int profiles_len = xr_has_bd_controllers ? len(profiles) : 1;
	int profiles_accepted = 0;
	for (int i = 0; i < profiles_len; i++) {
		XrPath profile;
		xr_check(xrStringToPath(xr_instance, profiles[i], &profile), "profile path");
		XrInteractionProfileSuggestedBinding suggested = {
			.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
			.interactionProfile = profile,
			.countSuggestedBindings = len(quest_actions),
			.suggestedBindings = bindings,
		};
		XrResult res = xrSuggestInteractionProfileBindings(xr_instance, &suggested);
		printf("controller profile %s: %s\n", profiles[i], XR_SUCCEEDED(res) ? "ok" : "not supported");
		if (XR_SUCCEEDED(res)) {
			profiles_accepted++;
		}
	}
	error_if(!profiles_accepted, "No supported controller profile");

	XrSessionActionSetsAttachInfo attach = {
		.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
		.countActionSets = 1,
		.actionSets = &xr_action_set,
	};
	xr_check(xrAttachSessionActionSets(xr_session, &attach), "attach action sets");
}

static void platform_xr_update_input(bool focused) {
	if (focused) {
		XrActiveActionSet active = {.actionSet = xr_action_set, .subactionPath = XR_NULL_PATH};
		XrActionsSyncInfo sync = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active};
		xrSyncActions(xr_session, &sync);
	}

	for (int i = 0; i < len(quest_actions); i++) {
		quest_action_t *qa = &quest_actions[i];
		XrActionStateGetInfo get = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = qa->action};

		if (qa->kind == ACTION_BOOL) {
			XrActionStateBoolean s = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
			bool down = focused && XR_SUCCEEDED(xrGetActionStateBoolean(xr_session, &get, &s)) && s.isActive && s.currentState;
			input_set_button_state(qa->button, down ? 1.0 : 0.0);
		}
		else if (qa->kind == ACTION_FLOAT) {
			XrActionStateFloat s = {.type = XR_TYPE_ACTION_STATE_FLOAT};
			float v = (focused && XR_SUCCEEDED(xrGetActionStateFloat(xr_session, &get, &s)) && s.isActive) ? s.currentState : 0;
			input_set_button_state(qa->button, v);
		}
		else {
			XrActionStateVector2f s = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
			vec2_t v = vec2(0, 0);
			if (focused && XR_SUCCEEDED(xrGetActionStateVector2f(xr_session, &get, &s)) && s.isActive) {
				v = vec2(s.currentState.x, s.currentState.y);
			}
			// OpenXR y is up
			input_set_button_state(qa->button + 0, max(v.y, 0)); // UP
			input_set_button_state(qa->button + 1, max(-v.y, 0)); // DOWN
			input_set_button_state(qa->button + 2, max(-v.x, 0)); // LEFT
			input_set_button_state(qa->button + 3, max(v.x, 0)); // RIGHT
		}
	}
}



// -----------------------------------------------------------------------------
// Frame

static bool platform_xr_poll_events(void) {
	XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(xr_instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)&event;
			xr_session_state = changed->state;
			printf("session state %d\n", xr_session_state);

			if (xr_session_state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo begin = {
					.type = XR_TYPE_SESSION_BEGIN_INFO,
					.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				};
				xr_check(xrBeginSession(xr_session, &begin), "xrBeginSession");
				xr_session_running = true;
				platform_xr_set_refresh_rate();
			}
			else if (xr_session_state == XR_SESSION_STATE_STOPPING) {
				xrEndSession(xr_session);
				xr_session_running = false;
			}
			else if (
				xr_session_state == XR_SESSION_STATE_EXITING ||
				xr_session_state == XR_SESSION_STATE_LOSS_PENDING
			) {
				return false;
			}
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			return false;
		}
		event = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
	}
	return true;
}

static uint32_t swapchain_acquire(swapchain_t *sc) {
	uint32_t index;
	XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	xr_check(xrAcquireSwapchainImage(sc->handle, &acquire, &index), "acquire image");
	XrSwapchainImageWaitInfo wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
	xr_check(xrWaitSwapchainImage(sc->handle, &wait), "wait image");
	return index;
}

static void swapchain_release(swapchain_t *sc) {
	XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xr_check(xrReleaseSwapchainImage(sc->handle, &release), "release image");
}

// View matrix of an eye relative to the reference space, in game units
static mat4_t eye_view_mat(XrPosef pose) {
	float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
	float r[3][3] = {
		{1 - 2*(y*y + z*z), 2*(x*y - w*z),     2*(x*z + w*y)},
		{2*(x*y + w*z),     1 - 2*(x*x + z*z), 2*(y*z - w*x)},
		{2*(x*z - w*y),     2*(y*z + w*x),     1 - 2*(x*x + y*y)},
	};
	float p[3] = {
		pose.position.x * RENDER_VR_UNITS_PER_METER,
		pose.position.y * RENDER_VR_UNITS_PER_METER,
		pose.position.z * RENDER_VR_UNITS_PER_METER,
	};

	// Inverse of the pose: transposed rotation, rotated negative translation
	mat4_t m = mat4_identity();
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			m.m[col * 4 + row] = r[col][row];
		}
		m.m[12 + row] = -(r[0][row] * p[0] + r[1][row] * p[1] + r[2][row] * p[2]);
	}
	return m;
}

static void platform_xr_frame(void) {
	XrFrameWaitInfo wait_info = {.type = XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
	xr_check(xrWaitFrame(xr_session, &wait_info, &frame_state), "xrWaitFrame");

	XrFrameBeginInfo begin_info = {.type = XR_TYPE_FRAME_BEGIN_INFO};
	xr_check(xrBeginFrame(xr_session, &begin_info), "xrBeginFrame");

	platform_xr_update_input(xr_session_state == XR_SESSION_STATE_FOCUSED);
	system_update();

	// Only races are shown in stereo; everything else uses a flat screen
	bool stereo = game_get_scene() == GAME_SCENE_RACE;

	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t layers_len = 0;
	XrCompositionLayerProjectionView projection_views[2];
	XrCompositionLayerProjection projection_layer;
	XrCompositionLayerQuad quad_layer;

	if (frame_state.shouldRender) {
		XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
		XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
		XrViewLocateInfo locate_info = {
			.type = XR_TYPE_VIEW_LOCATE_INFO,
			.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			.displayTime = frame_state.predictedDisplayTime,
			.space = xr_local_space,
		};
		uint32_t views_len = 0;
		xr_check(xrLocateViews(xr_session, &locate_info, &view_state, 2, &views_len, views), "xrLocateViews");
		bool views_valid =
			(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
			(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

		if (stereo && views_valid) {
			for (int i = 0; i < 2; i++) {
				swapchain_t *sc = &eye_swapchains[i];
				uint32_t index = swapchain_acquire(sc);
				mat4_t view = eye_view_mat(views[i].pose);
				render_vr_fov_t fov = {
					tanf(views[i].fov.angleLeft), tanf(views[i].fov.angleRight),
					tanf(views[i].fov.angleUp), tanf(views[i].fov.angleDown)
				};
				render_vr_draw_eye(sc->fbos[index], sc->size, &view, fov);
				swapchain_release(sc);

				projection_views[i] = (XrCompositionLayerProjectionView){
					.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
					.pose = views[i].pose,
					.fov = views[i].fov,
					.subImage = {
						.swapchain = sc->handle,
						.imageRect = {.offset = {0, 0}, .extent = {sc->size.x, sc->size.y}},
					},
				};
			}
			projection_layer = (XrCompositionLayerProjection){
				.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
				.space = xr_local_space,
				.viewCount = 2,
				.views = projection_views,
			};
			layers[layers_len++] = (XrCompositionLayerBaseHeader *)&projection_layer;
		}

		uint32_t index = swapchain_acquire(&screen_swapchain);
		render_vr_draw_screen(screen_swapchain.fbos[index], stereo);
		swapchain_release(&screen_swapchain);

		quad_layer = (XrCompositionLayerQuad){
			.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
			.layerFlags = stereo ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0,
			.space = xr_local_space,
			.eyeVisibility = XR_EYE_VISIBILITY_BOTH,
			.subImage = {
				.swapchain = screen_swapchain.handle,
				.imageRect = {.offset = {0, 0}, .extent = {SCREEN_WIDTH, SCREEN_HEIGHT}},
			},
			.pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, -SCREEN_DISTANCE}},
			.size = {SCREEN_WORLD_WIDTH, SCREEN_WORLD_HEIGHT},
		};
		layers[layers_len++] = (XrCompositionLayerBaseHeader *)&quad_layer;
	}

	XrFrameEndInfo end_info = {
		.type = XR_TYPE_FRAME_END_INFO,
		.displayTime = frame_state.predictedDisplayTime,
		.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		.layerCount = layers_len,
		.layers = layers,
	};
	xr_check(xrEndFrame(xr_session, &end_info), "xrEndFrame");
}



// -----------------------------------------------------------------------------
// Android lifecycle

static void platform_handle_app_cmd(struct android_app *app, int32_t cmd) {
	switch (cmd) {
		case APP_CMD_RESUME:
			app_resumed = true;
			platform_audio_set_playing(true);
			break;
		case APP_CMD_PAUSE:
			app_resumed = false;
			platform_audio_set_playing(false);
			break;
		case APP_CMD_DESTROY:
			wants_to_exit = true;
			break;
		default:
			break;
	}
}

static bool platform_pump_android_events(void) {
	for (;;) {
		// Block while there is nothing to render
		bool idle = !app_resumed && !xr_session_running && !android_app->destroyRequested;
		int events;
		struct android_poll_source *source;
		int res = ALooper_pollOnce(idle ? -1 : 0, NULL, &events, (void **)&source);
		if (res < 0 && res != ALOOPER_POLL_CALLBACK) {
			break;
		}
		if (source) {
			source->process(android_app, source);
		}
		if (android_app->destroyRequested) {
			return false;
		}
	}
	return true;
}

void android_main(struct android_app *app) {
	android_app = app;
	app->onAppCmd = platform_handle_app_cmd;
	log_redirect_stdout();

	// Game data lives in the app's external files dir, e.g.
	// /sdcard/Android/data/online.portroyale.wipeoutrewritevr/files/wipeout/
	const char *files_dir = app->activity->externalDataPath;
	path_assets = mem_bump(strlen(files_dir) + 2);
	strcpy(path_assets, files_dir);
	strcat(path_assets, "/");

	// NativeActivity doesn't create this dir. Creating it from here (rather
	// than through adb) makes it owned by the app.
	mkdir_parents(path_assets);
	path_userdata = path_assets;
	temp_path = mem_bump(strlen(path_assets) + 64);
	printf("asset path: %s\n", path_assets);
	platform_import_data_tar();

	JNIEnv *env;
	(*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL);

	platform_egl_init();
	platform_xr_create_instance();
	platform_xr_create_session();
	platform_xr_create_swapchains();
	platform_xr_create_actions();
	platform_audio_open();

	system_init();

	while (!wants_to_exit) {
		if (!platform_pump_android_events()) {
			break;
		}
		if (!platform_xr_poll_events()) {
			break;
		}
		if (!xr_session_running) {
			continue;
		}
		platform_xr_frame();
	}

	system_cleanup();

	if (audio_stream) {
		AAudioStream_close(audio_stream);
	}
	xrDestroySession(xr_session);
	xrDestroyInstance(xr_instance);
	platform_egl_cleanup();

	(*app->activity->vm)->DetachCurrentThread(app->activity->vm);
	ANativeActivity_finish(app->activity);

	// All game state is static; don't let a relaunch reuse this process
	exit(0);
}
