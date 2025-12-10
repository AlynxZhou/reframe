#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>

#define MVPRINT g_debug
#include "mvmath.h"

#include "rf-common.h"
#include "rf-converter.h"

struct _RfConverter {
	GObject parent_instance;
	RfConfig *config;
	char *card_path;
	GByteArray *buf;
	unsigned int gles_major;
	EGLDisplay display;
	EGLContext context;
	unsigned int program;
	unsigned int buffers[3];
	unsigned int vertex_array;
	unsigned int framebuffer;
	unsigned int texture;
	unsigned int rotation;
	unsigned int width;
	unsigned int height;
	bool running;
};
G_DEFINE_TYPE(RfConverter, rf_converter, G_TYPE_OBJECT)

static EGLDisplay _get_egl_display_from_drm_card(const char *card_path)
{
	if (card_path == NULL)
		return EGL_NO_DISPLAY;

	int max_devices = 0;
	int num_devices = 0;
	EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
	if (!eglQueryDevicesEXT(0, NULL, &max_devices)) {
		g_warning(
			"EGL: Failed to query max devices: %d.", eglGetError()
		);
		return EGL_NO_DISPLAY;
	}

	g_autofree EGLDeviceEXT *devices =
		g_malloc0_n(max_devices, sizeof(*devices));
	if (!eglQueryDevicesEXT(max_devices, devices, &num_devices)) {
		g_warning("EGL: Failed to query devices: %d.", eglGetError());
		return EGL_NO_DISPLAY;
	}

	for (size_t i = 0; i < num_devices; ++i) {
		const char *f = eglQueryDeviceStringEXT(
			devices[i], EGL_DRM_DEVICE_FILE_EXT
		);
		if (g_strcmp0(card_path, f) == 0) {
			device = devices[i];
			break;
		}
	}
	if (device == EGL_NO_DEVICE_EXT)
		return EGL_NO_DISPLAY;

	g_message("EGL: Got device for %s.", card_path);
	return eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, device, NULL);
}

static int _setup_egl(RfConverter *this)
{
	EGLint major;
	EGLint minor;
	EGLint count;
	EGLint n;
	EGLint size;
	EGLConfig config = NULL;
	EGLConfig *configs;
	// clang-format off
	EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
#ifdef __DEBUG__
		EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
#endif
		EGL_NONE
	};
	// clang-format on

	this->display = _get_egl_display_from_drm_card(this->card_path);
	if (this->display == EGL_NO_DISPLAY) {
		g_warning(
			"EGL: Failed to get platform display. Fallback to default display (may not work if you have more than one GPUs)."
		);
		this->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	if (this->display == EGL_NO_DISPLAY) {
		g_warning(
			"EGL: Failed to get default display: %d.", eglGetError()
		);
		return -1;
	}

	if (!eglInitialize(this->display, &major, &minor)) {
		g_warning("EGL: Failed to initialize: %d.", eglGetError());
		return -2;
	}
	g_debug("EGL: Got version major %d and minor %d.", major, minor);

	eglBindAPI(EGL_OPENGL_ES_API);

	eglGetConfigs(this->display, NULL, 0, &count);
	configs = g_malloc_n(count, sizeof(*configs));
	if (!eglChooseConfig(
		    this->display, config_attribs, configs, count, &n
	    )) {
		config_attribs[1] = EGL_OPENGL_ES2_BIT;
		eglChooseConfig(
			this->display, config_attribs, configs, count, &n
		);
	}
	for (int i = 0; i < n; ++i) {
		eglGetConfigAttrib(
			this->display, configs[i], EGL_BUFFER_SIZE, &size
		);
		if (size == 32) {
			config = configs[i];
			break;
		}
	}

	this->gles_major = 3;
	this->context = eglCreateContext(
		this->display, config, EGL_NO_CONTEXT, context_attribs
	);
	if (this->context == EGL_NO_CONTEXT) {
		g_message(
			"EGL: Failed to create GLES v3 context, fallback to GLES v2."
		);
		this->gles_major = 2;
		context_attribs[1] = 2;
		this->context = eglCreateContext(
			this->display, config, EGL_NO_CONTEXT, context_attribs
		);
	}
	if (this->context == EGL_NO_CONTEXT) {
		g_warning("EGL: Failed to create context: %d.", eglGetError());
		return -3;
	}

	g_free(configs);

	return 0;
}

static void _clean_egl(RfConverter *this)
{
	if (this->context != EGL_NO_CONTEXT) {
		eglDestroyContext(this->display, this->context);
		this->context = EGL_NO_CONTEXT;
	}
	if (this->display != EGL_NO_DISPLAY) {
		eglTerminate(this->display);
		this->display = EGL_NO_DISPLAY;
	}
	// eglReleaseThread();
}

#ifdef __DEBUG__
static void _gl_message(
	unsigned int source,
	unsigned int type,
	unsigned int id,
	unsigned int severity,
	int length,
	const char *message,
	const void *user_param
)
{
	if (type == GL_DEBUG_TYPE_ERROR)
		g_warning("GL: Failed to call command: %s.", message);
}
#endif

static unsigned int _make_shader(GLenum type, const char *s)
{
	unsigned int shader = glCreateShader(type);
	if (shader == 0)
		return 0;
	glShaderSource(shader, 1, &s, NULL);
	glCompileShader(shader);
	int compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == 0) {
		glDeleteShader(shader);
		g_debug("%s", s);
		g_warning("GL: Failed to compile shader.");
		return 0;
	}
	return shader;
}

static unsigned int _make_program(const char *vs, const char *fs)
{
	unsigned int v = _make_shader(GL_VERTEX_SHADER, vs);
	unsigned int f = _make_shader(GL_FRAGMENT_SHADER, fs);
	if (v == 0 || f == 0)
		return 0;
	unsigned int program = glCreateProgram();
	if (program == 0)
		return 0;
	glAttachShader(program, v);
	glAttachShader(program, f);
	glLinkProgram(program);
	glDeleteShader(v);
	glDeleteShader(f);
	int linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked == 0) {
		glDeleteProgram(program);
		g_warning("GL: Failed to link program.");
		return 0;
	}
	return program;
}

static void _bind_buffers(RfConverter *this)
{
	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[0]);
	glVertexAttribPointer(
		glGetAttribLocation(this->program, "in_position"),
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(float),
		0
	);
	glEnableVertexAttribArray(
		glGetAttribLocation(this->program, "in_position")
	);

	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[1]);
	glVertexAttribPointer(
		glGetAttribLocation(this->program, "in_coordinate"),
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(float),
		0
	);
	glEnableVertexAttribArray(
		glGetAttribLocation(this->program, "in_coordinate")
	);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->buffers[2]);
}

static void _unbind_buffers(RfConverter *this)
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(
		glGetAttribLocation(this->program, "in_position")
	);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(
		glGetAttribLocation(this->program, "in_coordinate")
	);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static int _setup_gl(RfConverter *this)
{
	if (!eglMakeCurrent(
		    this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->context
	    )) {
		g_warning(
			"EGL: Failed to make context current: %d.",
			eglGetError()
		);
		return -4;
	}

#ifdef __DEBUG__
	glEnable(GL_DEBUG_OUTPUT);
	glDebugMessageCallback(_gl_message, NULL);
#endif
	glEnable(GL_CULL_FACE);
	// glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(true);

	if (this->gles_major >= 3) {
		const char vs[] =
			"#version 300 es\n"
			"uniform mat4 mvp;\n"
			"in vec2 in_position;\n"
			"in vec2 in_coordinate;\n"
			"out vec2 pass_coordinate;\n"
			"void main() {\n"
			"	gl_Position = mvp * vec4(in_position, 0.0f, 1.0f);\n"
			"	pass_coordinate = in_coordinate;\n"
			"}\n";
		const char fs[] =
			"#version 300 es\n"
			"#ifdef GL_OES_EGL_image_external_essl3\n"
			"#extension GL_OES_EGL_image_external_essl3 : require\n"
			"#else\n"
			"#extension GL_OES_EGL_image_external : require\n"
			"#endif\n"
			"precision mediump float;\n"
			"uniform samplerExternalOES image;\n"
			"in vec2 pass_coordinate;\n"
			"out vec4 out_color;\n"
			"void main() {\n"
			"	out_color = texture(image, pass_coordinate);\n"
			"}\n";
		this->program = _make_program(vs, fs);
	} else {
		const char vs[] =
			"#version 100\n"
			"uniform mat4 mvp;\n"
			"attribute vec2 in_position;\n"
			"attribute vec2 in_coordinate;\n"
			"varying vec2 pass_coordinate;\n"
			"void main() {\n"
			"	gl_Position = mvp * vec4(in_position, 0.0, 1.0);\n"
			"	pass_coordinate = in_coordinate;\n"
			"}\n";
		const char fs[] =
			"#version 100\n"
			"#extension GL_OES_EGL_image_external : require\n"
			"precision mediump float;\n"
			"uniform samplerExternalOES image;\n"
			"varying vec2 pass_coordinate;\n"
			"void main() {\n"
			"	gl_FragColor = texture2D(image, pass_coordinate);\n"
			"}\n";
		this->program = _make_program(vs, fs);
	}
	if (this->program == 0)
		return -5;

	glUseProgram(this->program);
	// Use texture 0 for this sampler. This only needs to be done once.
	glUniform1i(glGetUniformLocation(this->program, "image"), 0);
	glUseProgram(0);

	glGenBuffers(3, this->buffers);

	// clang-format off
	const float vertices[] = {
		// x, y
		// top left
		0.0f, 1.0f,
		// top right
		1.0f, 1.0f,
		// bottom right
		1.0f, 0.0f,
		// bottom left
		0.0f, 0.0f
	};
	// clang-format on
	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[0]);
	glBufferData(
		GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW
	);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// clang-format off
	float coordinates[] = {
		// u, v
		// top left
		0.0f, 1.0f,
		// top right
		1.0f, 1.0f,
		// bottom right
		1.0f, 0.0f,
		// bottom left
		0.0f, 0.0f
	};
	// clang-format on
	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[1]);
	glBufferData(
		GL_ARRAY_BUFFER, sizeof(coordinates), coordinates, GL_STATIC_DRAW
	);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// clang-format off
	// Counter-clockwise is front!
	const unsigned char indices[] = {
		0, 3, 1,
		2, 1, 3
	};
	// clang-format on
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->buffers[2]);
	glBufferData(
		GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW
	);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	if (this->gles_major >= 3) {
		glGenVertexArrays(1, &this->vertex_array);

		glBindVertexArray(this->vertex_array);
		_bind_buffers(this);
		glBindVertexArray(0);
	}

	glGenFramebuffers(1, &this->framebuffer);

	glClearColor(0.3f, 0.3f, 0.3f, 1.0f);

	return 0;
}

static void _clean_gl(RfConverter *this)
{
	if (this->texture != 0) {
		glDeleteTextures(1, &this->texture);
		this->texture = 0;
	}
	if (this->framebuffer != 0) {
		glDeleteFramebuffers(1, &this->framebuffer);
		this->framebuffer = 0;
	}
	if (this->vertex_array != 0) {
		glDeleteVertexArrays(1, &this->vertex_array);
		this->vertex_array = 0;
	}
	if (this->buffers[0] != 0) {
		glDeleteBuffers(3, this->buffers);
		this->buffers[0] = 0;
		this->buffers[1] = 0;
		this->buffers[2] = 0;
	}
	if (this->program != 0) {
		glDeleteProgram(this->program);
		this->program = 0;
	}
}

static void _finalize(GObject *o)
{
	RfConverter *this = RF_CONVERTER(o);

	rf_converter_stop(this);

	G_OBJECT_CLASS(rf_converter_parent_class)->finalize(o);
}

static void rf_converter_init(RfConverter *this)
{
	this->config = NULL;
	this->card_path = NULL;
	this->buf = NULL;
	this->gles_major = 3;
	this->display = EGL_NO_DISPLAY;
	this->context = EGL_NO_CONTEXT;
	this->program = 0;
	this->buffers[0] = 0;
	this->buffers[1] = 0;
	this->buffers[2] = 0;
	this->vertex_array = 0;
	this->framebuffer = 0;
	this->texture = 0;
	this->width = 0;
	this->height = 0;
	this->rotation = 0;
	this->running = false;
}

static void rf_converter_class_init(RfConverterClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = _finalize;
}

RfConverter *rf_converter_new(RfConfig *config)
{
	RfConverter *this = g_object_new(RF_TYPE_CONVERTER, NULL);
	this->config = config;
	return this;
}

void rf_converter_set_card_path(RfConverter *this, const char *card_path)
{
	g_return_if_fail(RF_IS_CONVERTER(this));
	g_return_if_fail(card_path != NULL);

	g_clear_pointer(&this->card_path, g_free);
	this->card_path = g_strdup(card_path);
}

int rf_converter_start(RfConverter *this)
{
	g_return_val_if_fail(RF_IS_CONVERTER(this), -1);

	if (this->running)
		return 0;

	if (this->card_path == NULL) {
		g_warning("EGL: Card path is not set, fallback to config.");
		this->card_path = rf_config_get_card_path(this->config);
	}
	this->rotation = rf_config_get_rotation(this->config);
	g_message("GL: Got screen rotation %u.", this->rotation);
	this->width = 0;
	this->height = 0;
	int ret = 0;
	ret = _setup_egl(this);
	if (ret < 0)
		goto out;
	ret = _setup_gl(this);
	if (ret < 0)
		goto out;

	this->running = true;

out:
	if (ret < 0) {
		_clean_gl(this);
		_clean_egl(this);
	}
	return ret;
}

void rf_converter_stop(RfConverter *this)
{
	g_return_if_fail(RF_IS_CONVERTER(this));

	if (!this->running)
		return;

	this->running = false;

	g_clear_pointer(&this->buf, g_byte_array_unref);
	g_clear_pointer(&this->card_path, g_free);
	_clean_gl(this);
	_clean_egl(this);
}

static inline void _append_attrib(GArray *a, EGLAttrib k, EGLAttrib v)
{
	g_array_append_val(a, k);
	g_array_append_val(a, v);
}

static void _gen_texture(RfConverter *this)
{
	g_debug("GL: Generating new texture for width %u and height %u.",
		this->width,
		this->height);

	if (this->texture != 0)
		glDeleteTextures(1, &this->texture);

	glBindFramebuffer(GL_FRAMEBUFFER, this->framebuffer);
	glGenTextures(1, &this->texture);
	glBindTexture(GL_TEXTURE_2D, this->texture);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA,
		this->width,
		this->height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		NULL
	);
	glFramebufferTexture2D(
		GL_FRAMEBUFFER,
		GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D,
		this->texture,
		0
	);
	GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &draw_buffer);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static EGLImage _make_image(EGLDisplay *display, const RfBuffer *b)
{
	EGLAttrib fd_keys[RF_MAX_FDS] = { EGL_DMA_BUF_PLANE0_FD_EXT,
					  EGL_DMA_BUF_PLANE1_FD_EXT,
					  EGL_DMA_BUF_PLANE2_FD_EXT,
					  EGL_DMA_BUF_PLANE3_FD_EXT };
	EGLAttrib offset_keys[RF_MAX_FDS] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT,
					      EGL_DMA_BUF_PLANE1_OFFSET_EXT,
					      EGL_DMA_BUF_PLANE2_OFFSET_EXT,
					      EGL_DMA_BUF_PLANE3_OFFSET_EXT };
	EGLAttrib pitch_keys[RF_MAX_FDS] = { EGL_DMA_BUF_PLANE0_PITCH_EXT,
					     EGL_DMA_BUF_PLANE1_PITCH_EXT,
					     EGL_DMA_BUF_PLANE2_PITCH_EXT,
					     EGL_DMA_BUF_PLANE3_PITCH_EXT };
	EGLAttrib modlo_keys[RF_MAX_FDS] = {
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
	};
	EGLAttrib modhi_keys[RF_MAX_FDS] = {
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
	};
	GArray *image_attribs = g_array_new(false, false, sizeof(EGLAttrib));
	_append_attrib(image_attribs, EGL_WIDTH, b->md.width);
	_append_attrib(image_attribs, EGL_HEIGHT, b->md.height);
	_append_attrib(image_attribs, EGL_LINUX_DRM_FOURCC_EXT, b->md.fourcc);
	for (int i = 0; i < b->md.length; ++i) {
		_append_attrib(image_attribs, fd_keys[i], b->fds[i]);
		_append_attrib(image_attribs, offset_keys[i], b->md.offsets[i]);
		_append_attrib(image_attribs, pitch_keys[i], b->md.pitches[i]);
		if (b->md.modifier != DRM_FORMAT_MOD_INVALID) {
			_append_attrib(
				image_attribs,
				modlo_keys[i],
				(b->md.modifier & 0xFFFFFFFF)
			);
			_append_attrib(
				image_attribs,
				modhi_keys[i],
				(b->md.modifier >> 32)
			);
		}
	}
	EGLAttrib key = EGL_NONE;
	g_array_append_val(image_attribs, key);
	// EGL_NO_CONTEXT must be used here according to the docs.
	//
	// See <https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt>.
	EGLImage image = eglCreateImage(
		display,
		EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT,
		(EGLClientBuffer)NULL,
		(EGLAttrib *)image_attribs->data
	);
	g_array_free(image_attribs, true);
	return image;
}

static void _draw_begin(RfConverter *this)
{
	glViewport(0, 0, this->width, this->height);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glUseProgram(this->program);
	if (this->gles_major >= 3)
		glBindVertexArray(this->vertex_array);
	else
		_bind_buffers(this);
}

static void _draw_rect(
	RfConverter *this,
	EGLImage image,
	int64_t x,
	int64_t y,
	uint32_t w,
	uint32_t h,
	uint32_t z,
	uint32_t frame_width,
	uint32_t frame_height
)
{
	unsigned int texture;
	glGenTextures(1, &texture);
	glActiveTexture(GL_TEXTURE0);
	// While `GL_TEXTURE_2D` does work for most cases, it won't work with
	// NVIDIA and linear modifier (which is used by TTY), we have to use
	// `GL_TEXTURE_EXTERNAL_OES`.
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
	// Setting sampling filter to scaling texture automatically.
	glTexParameteri(
		GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE
	);
	glTexParameteri(
		GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE
	);
	glTexParameteri(
		GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR
	);
	glTexParameteri(
		GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR
	);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
	// g_debug("glEGLImageTargetTexture2DOES: %#x", glGetError());

	mat4 model =
		m4multiply(m4translate(v3s(x, y, z)), m4scale(v3s(w, h, 1.0f)));
	mat4 view = m4camera(
		v3s(0.0f, 0.0f, 0.0f),
		v3s(0.0f, 0.0f, 1.0f),
		v3s(0.0f, 1.0f, 0.0f)
	);
	mat4 projection =
		m4ortho(0.0f, frame_width, frame_height, 0.0f, 0.1f, 100.0f);
	mat4 mvp = m4multiply(projection, m4multiply(view, model));
	// Rotating monitor is just the same as rotating the whole world.
	mvp = m4multiply(
		m4rotate(v3s(0.0f, 0.0f, -1.0f), sradians(this->rotation)), mvp
	);

	glUniformMatrix4fv(
		glGetUniformLocation(this->program, "mvp"), 1, false, MARRAY(mvp)
	);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, 0);
	// g_debug("glDrawElements: %#x", glGetError());

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glDeleteTextures(1, &texture);
}

static void _draw_buffer(
	RfConverter *this,
	const RfBuffer *b,
	uint32_t z,
	uint32_t frame_width,
	uint32_t frame_height
)
{
	EGLImage image = _make_image(this->display, b);
	if (image == EGL_NO_IMAGE) {
		g_warning("EGL: Failed to create image: %d.", eglGetError());
		return;
	}
	_draw_rect(
		this,
		image,
		b->md.crtc_x,
		b->md.crtc_y,
		b->md.width,
		b->md.height,
		z,
		frame_width,
		frame_height
	);
	eglDestroyImage(this->display, image);
}

static void _draw_end(RfConverter *this)
{
	if (this->gles_major >= 3)
		glBindVertexArray(0);
	else
		_unbind_buffers(this);
	glUseProgram(0);
}

GByteArray *rf_converter_convert(
	RfConverter *this,
	size_t length,
	const RfBuffer *bufs,
	unsigned int width,
	unsigned int height
)
{
	g_return_val_if_fail(RF_IS_CONVERTER(this), NULL);
	g_return_val_if_fail(length >= 1 && length <= RF_MAX_BUFS, NULL);
	g_return_val_if_fail(bufs != NULL, NULL);
	g_return_val_if_fail(width > 0 && height > 0, NULL);

	if (!this->running)
		return NULL;

	if (!eglMakeCurrent(
		    this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->context
	    )) {
		g_warning(
			"EGL: Failed to make context current: %d.",
			eglGetError()
		);
		return NULL;
	}

	GByteArray *res = NULL;

	if (this->width != width || this->height != height) {
		this->width = width;
		this->height = height;
		_gen_texture(this);
		g_clear_pointer(&this->buf, g_byte_array_unref);
		this->buf = g_byte_array_sized_new(
			RF_BYTES_PER_PIXEL * this->width * this->height
		);
	}

	const RfBuffer *primary = &bufs[0];
	// Currently we assume primary size is always the same as monitor
	// size. However, maybe we should use CRTC size actually?
	uint32_t frame_width = primary->md.width;
	uint32_t frame_height = primary->md.height;

	glBindFramebuffer(GL_FRAMEBUFFER, this->framebuffer);

	_draw_begin(this);

	for (size_t i = 0; i < length; ++i)
		_draw_buffer(this, &bufs[i], length - i, frame_width, frame_height);

	_draw_end(this);

	glPixelStorei(GL_PACK_ALIGNMENT, RF_BYTES_PER_PIXEL);
	// OpenGL ES only ensures `GL_RGBA` and `GL_RGB`, `GL_BGRA` is optional.
	// But luckily LibVNCServer accepts RGBA by default.
	glReadPixels(
		0,
		0,
		this->width,
		this->height,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		this->buf->data
	);
	// g_debug("glReadPixels: %#x", glGetError());
	if (glGetError() == GL_NO_ERROR)
		res = g_byte_array_ref(this->buf);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return res;
}
