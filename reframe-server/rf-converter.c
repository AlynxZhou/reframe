#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>

#include "rf-common.h"
#include "rf-converter.h"

struct _RfConverter {
	GObject parent_instance;
	RfConfig *config;
	GByteArray *buf;
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
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
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

	this->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (this->display == EGL_NO_DISPLAY) {
		g_warning("EGL: Failed to get display: %d.", eglGetError());
		return -1;
	}

	if (!eglInitialize(this->display, &major, &minor)) {
		g_warning("EGL: Failed to initialize: %d.", eglGetError());
		return -2;
	}
	g_debug("EGL: Version is major %d, minor %d.", major, minor);

	eglBindAPI(EGL_OPENGL_ES_API);

	eglGetConfigs(this->display, NULL, 0, &count);
	configs = g_malloc_n(count, sizeof(*configs));
	eglChooseConfig(this->display, config_attribs, configs, count, &n);
	for (int i = 0; i < n; ++i) {
		eglGetConfigAttrib(
			this->display, configs[i], EGL_BUFFER_SIZE, &size
		);
		if (size == 32) {
			config = configs[i];
			break;
		}
	}

	this->context = eglCreateContext(
		this->display, config, EGL_NO_CONTEXT, context_attribs
	);
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

static void _gen_texture(RfConverter *this)
{
	g_debug("GL: Generating new texture for width %d and height %d.",
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
	// glEnable(GL_CULL_FACE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(false);

	const char vs[] =
		"#version 300 es\n"
		"in vec2 in_position;\n"
		"in vec2 in_coordinate;\n"
		"out vec2 pass_coordinate;\n"
		"void main() {\n"
		"	gl_Position = vec4(in_position, 0.0f, 1.0f);\n"
		"	pass_coordinate = in_coordinate;\n"
		"}\n";
	const char fs[] =
		"#version 300 es\n"
		"precision mediump float;\n"
		"uniform sampler2D image;\n"
		"in vec2 pass_coordinate;\n"
		"out vec4 out_color;\n"
		"void main() {\n"
		"	out_color = texture(image, pass_coordinate);\n"
		"}\n";
	this->program = _make_program(vs, fs);
	if (this->program == 0)
		return -5;

	glGenBuffers(3, this->buffers);

	// clang-format off
	const float vertices[] = {
		// x, y
		// top left
		-1.0f, 1.0f,
		// top right
		1.0f, 1.0f,
		// bottom right
		1.0f, -1.0f,
		// bottom left
		-1.0f, -1.0f
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
	// We only support 90-degree rotation so we don't need a rotate matrix.
	for (unsigned int r = this->rotation; r > 0; r -= 90) {
		float u = coordinates[0];
		float v = coordinates[1];
		coordinates[0] = coordinates[2];
		coordinates[1] = coordinates[3];
		coordinates[2] = coordinates[4];
		coordinates[3] = coordinates[5];
		coordinates[4] = coordinates[6];
		coordinates[5] = coordinates[7];
		coordinates[6] = u;
		coordinates[7] = v;
	}
	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[1]);
	glBufferData(
		GL_ARRAY_BUFFER, sizeof(coordinates), coordinates, GL_STATIC_DRAW
	);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// clang-format off
	// Counter-clockwise is front!
	const unsigned int indices[] = {
		0, 3, 1,
		2, 1, 3
	};
	// clang-format on
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->buffers[2]);
	glBufferData(
		GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW
	);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glGenVertexArrays(1, &this->vertex_array);

	glBindVertexArray(this->vertex_array);

	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[0]);
	glVertexAttribPointer(
		glGetAttribLocation(this->program, "in_position"),
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(*vertices),
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
		2 * sizeof(*coordinates),
		0
	);
	glEnableVertexAttribArray(
		glGetAttribLocation(this->program, "in_coordinate")
	);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->buffers[2]);

	glBindVertexArray(0);

	glGenFramebuffers(1, &this->framebuffer);

	_gen_texture(this);

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
	this->buf = NULL;
	this->context = EGL_NO_CONTEXT;
	this->display = EGL_NO_DISPLAY;
	this->program = 0;
	this->buffers[0] = 0;
	this->buffers[1] = 0;
	this->buffers[2] = 0;
	this->vertex_array = 0;
	this->framebuffer = 0;
	this->texture = 0;
	this->width = RF_DEFAULT_WIDTH;
	this->height = RF_DEFAULT_HEIGHT;
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

int rf_converter_start(RfConverter *this)
{
	g_return_val_if_fail(RF_IS_CONVERTER(this), -1);

	if (this->running)
		return 0;

	// Keep all size initialization the same.
	this->rotation = rf_config_get_rotation(this->config);
	g_debug("GL: Got screen rotation %u.", this->rotation);
	// Assuming most monitors are landscape.
	this->width = RF_DEFAULT_WIDTH;
	this->height = RF_DEFAULT_HEIGHT;
	if (this->rotation % 180 != 0) {
		this->height = RF_DEFAULT_WIDTH;
		this->width = RF_DEFAULT_HEIGHT;
	}
	int ret = 0;
	ret = _setup_egl(this);
	if (ret < 0)
		goto clean_egl;
	ret = _setup_gl(this);
	if (ret < 0)
		goto clean_gl;
	this->buf = g_byte_array_sized_new(
		RF_BYTES_PER_PIXEL * this->width * this->height
	);

	this->running = true;
	goto out;

clean_gl:
	_clean_gl(this);
clean_egl:
	_clean_egl(this);
out:
	return ret;
}

void rf_converter_stop(RfConverter *this)
{
	g_return_if_fail(RF_IS_CONVERTER(this));

	if (!this->running)
		return;

	this->running = false;

	g_clear_pointer(&this->buf, g_byte_array_unref);
	_clean_gl(this);
	_clean_egl(this);
}

static inline void _append_attrib(GArray *a, EGLAttrib k, EGLAttrib v)
{
	g_array_append_val(a, k);
	g_array_append_val(a, v);
}

GByteArray *rf_converter_convert(
	RfConverter *this,
	const RfBuffer *b,
	unsigned int width,
	unsigned int height
)
{
	g_return_val_if_fail(RF_IS_CONVERTER(this), NULL);
	g_return_val_if_fail(b != NULL, NULL);
	g_return_val_if_fail(width > 0 && height > 0, NULL);

	if (!this->running)
		return NULL;

	EGLAttrib fd_keys[RF_MAX_PLANES] = { EGL_DMA_BUF_PLANE0_FD_EXT,
					     EGL_DMA_BUF_PLANE1_FD_EXT,
					     EGL_DMA_BUF_PLANE2_FD_EXT,
					     EGL_DMA_BUF_PLANE3_FD_EXT };
	EGLAttrib offset_keys[RF_MAX_PLANES] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT,
						 EGL_DMA_BUF_PLANE1_OFFSET_EXT,
						 EGL_DMA_BUF_PLANE2_OFFSET_EXT,
						 EGL_DMA_BUF_PLANE3_OFFSET_EXT };
	EGLAttrib pitch_keys[RF_MAX_PLANES] = { EGL_DMA_BUF_PLANE0_PITCH_EXT,
						EGL_DMA_BUF_PLANE1_PITCH_EXT,
						EGL_DMA_BUF_PLANE2_PITCH_EXT,
						EGL_DMA_BUF_PLANE3_PITCH_EXT };
	EGLAttrib modlo_keys[RF_MAX_PLANES] = {
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
	};
	EGLAttrib modhi_keys[RF_MAX_PLANES] = {
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
		this->display,
		EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT,
		(EGLClientBuffer)NULL,
		(EGLAttrib *)image_attribs->data
	);

	g_array_free(image_attribs, true);

	if (image == EGL_NO_IMAGE) {
		g_warning("EGL: Failed to create image: %d.", eglGetError());
		return NULL;
	}

	if (!eglMakeCurrent(
		    this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->context
	    )) {
		g_warning(
			"EGL: Failed to make context current: %d.",
			eglGetError()
		);
		return NULL;
	}

	if (this->width != width || this->height != height) {
		this->width = width;
		this->height = height;
		_gen_texture(this);
		g_clear_pointer(&this->buf, g_byte_array_unref);
		this->buf = g_byte_array_sized_new(
			RF_BYTES_PER_PIXEL * this->width * this->height
		);
	}

	unsigned int texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	// Setting sampling filter to scaling texture automatically.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
	// g_debug("glEGLImageTargetTexture2DOES: %#x", glGetError());

	// This would be the easiest way if I can use full OpenGL. However,
	// libGL will pulls libGLX, I don't want that. With OpenGL ES we don't
	// have `glGetTexImage()`, so we have to use framebuffers.
	// glGetTexImage(
	//	GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, this->buf->data
	// );

	glBindFramebuffer(GL_FRAMEBUFFER, this->framebuffer);
	glViewport(0, 0, this->width, this->height);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(this->program);
	glUniform1i(glGetUniformLocation(this->program, "image"), 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(this->vertex_array);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
	// g_debug("glDrawElements: %#x", glGetError());
	glBindVertexArray(0);
	glUseProgram(0);
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
	if (glGetError() != GL_NO_ERROR)
		return NULL;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &texture);

	eglDestroyImage(this->display, image);

	return g_byte_array_ref(this->buf);
}
