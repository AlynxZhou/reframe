#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>

#include "rf-common.h"
#include "rf-converter.h"

struct _RfConverter {
	GObject parent_instance;
	EGLDisplay display;
	EGLContext context;
	unsigned int program;
	unsigned int vertex_array;
	unsigned int framebuffer;
	unsigned int texture;
	unsigned int width;
	unsigned int height;
};
G_DEFINE_TYPE(RfConverter, rf_converter, G_TYPE_OBJECT)

static void _gen_texture(RfConverter *c)
{
	if (c->texture != 0)
		glDeleteTextures(1, &c->texture);

	glBindFramebuffer(GL_FRAMEBUFFER, c->framebuffer);
	glGenTextures(1, &c->texture);
	g_debug("glGenTextures: %#x", glGetError());
	glBindTexture(GL_TEXTURE_2D, c->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->width, c->height, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	g_debug("glTexImage2D: %#x", glGetError());
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, c->texture, 0);
	g_debug("glFramebufferTexture2D: %#x", glGetError());
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
		g_error("Failed to compile shader: %s.", s);
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
		return 0;
	}
	return program;
}

static void _init_egl(RfConverter *c)
{
	EGLint major;
	EGLint minor;
	EGLint count;
	EGLint n;
	EGLint size;
	EGLConfig config;
	EGLConfig *configs;
	EGLint config_attribs[] = { EGL_SURFACE_TYPE,
				    EGL_PBUFFER_BIT,
				    EGL_RED_SIZE,
				    8,
				    EGL_GREEN_SIZE,
				    8,
				    EGL_BLUE_SIZE,
				    8,
				    EGL_ALPHA_SIZE,
				    8,
				    EGL_RENDERABLE_TYPE,
				    EGL_OPENGL_ES3_BIT,
				    EGL_NONE };

	EGLint context_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
				     // EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
				     EGL_NONE };

	c->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (c->display == EGL_NO_DISPLAY) {
		g_error("Cannot get EGL display: %d.", eglGetError());
		exit(EXIT_FAILURE);
	}

	if (!eglInitialize(c->display, &major, &minor)) {
		g_error("Cannot initialize EGL: %d.", eglGetError());
		exit(EXIT_FAILURE);
	}
	eglBindAPI(EGL_OPENGL_ES_API);
#ifdef __DEBUG__
	g_debug("EGL version: major: %d, minor: %d.", major, minor);
#endif

	eglGetConfigs(c->display, NULL, 0, &count);
	configs = g_malloc_n(count, sizeof(*configs));
	eglChooseConfig(c->display, config_attribs, configs, count, &n);
	for (int i = 0; i < n; ++i) {
#ifdef __DEBUG__
		eglGetConfigAttrib(c->display, configs[i], EGL_ALPHA_SIZE,
				   &size);
		g_debug("Config %d: EGL_ALPHA_SIZE: %d.", i, size);
#endif
#ifdef __DEBUG__
		eglGetConfigAttrib(c->display, configs[i], EGL_RED_SIZE, &size);
		g_debug("Config %d: EGL_RED_SIZE: %d.", i, size);
#endif
#ifdef __DEBUG__
		eglGetConfigAttrib(c->display, configs[i], EGL_GREEN_SIZE,
				   &size);
		g_debug("Config %d: EGL_GREEN_SIZE: %d.", i, size);
#endif
#ifdef __DEBUG__
		eglGetConfigAttrib(c->display, configs[i], EGL_BLUE_SIZE,
				   &size);
		g_debug("Config %d: EGL_BLUE_SIZE: %d.", i, size);
#endif
		eglGetConfigAttrib(c->display, configs[i], EGL_BUFFER_SIZE,
				   &size);
		if (size == 32) {
			config = configs[i];
			break;
		}
	}

	c->context = eglCreateContext(c->display, config, EGL_NO_CONTEXT,
				      context_attribs);
	if (c->context == EGL_NO_CONTEXT) {
		g_error("Cannot create EGL context: %d.", eglGetError());
		exit(EXIT_FAILURE);
	}

	free(configs);
}

static void _init_gl(RfConverter *c)
{
	if (!eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    c->context)) {
		g_error("Cannot make current: %d.", eglGetError());
		// exit(EXIT_FAILURE);
	}

	// glEnable(GL_DEBUG_OUTPUT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
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
	c->program = _make_program(vs, fs);
	if (c->program == 0) {
		g_error("Failed to make program: %#x.", glGetError());
		exit(EXIT_FAILURE);
	}

	unsigned int buffers[3];
	glGenBuffers(3, buffers);

	const float vertices[] = { // x, y
				   // bottom left
				   -1.0f, -1.0f,
				   // top left
				   -1.0f, 1.0f,
				   // bottom right
				   1.0f, -1.0f,
				   // top right
				   1.0f, 1.0f
	};
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
		     GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	const float coordinates[] = { // u, v
				      // bottom left
				      0.0f, 0.0f,
				      // top left
				      0.0f, 1.0f,
				      // bottom right
				      1.0f, 0.0f,
				      // top right
				      1.0f, 1.0f
	};
	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(coordinates), coordinates,
		     GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	const unsigned int indices[] = { 0, 1, 2, 3, 2, 1 };
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
		     GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glGenVertexArrays(1, &c->vertex_array);
	g_debug("glGenVertexArrays: %#x", glGetError());

	glBindVertexArray(c->vertex_array);

	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glVertexAttribPointer(glGetAttribLocation(c->program, "in_position"), 2,
			      GL_FLOAT, GL_FALSE, 2 * sizeof(*vertices), 0);
	glEnableVertexAttribArray(
		glGetAttribLocation(c->program, "in_position"));

	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
	glVertexAttribPointer(glGetAttribLocation(c->program, "in_coordinate"),
			      2, GL_FLOAT, GL_FALSE, 2 * sizeof(*coordinates),
			      0);
	glEnableVertexAttribArray(
		glGetAttribLocation(c->program, "in_coordinate"));

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);

	glBindVertexArray(0);

	glGenFramebuffers(1, &c->framebuffer);
	g_debug("glGenFramebuffers: %#x", glGetError());

	_gen_texture(c);

	glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
}

static void _finalize(GObject *o)
{
	RfConverter *c = RF_CONVERTER(o);

	if (c->context != EGL_NO_CONTEXT) {
		eglDestroyContext(c->display, c->context);
		c->context = EGL_NO_CONTEXT;
	}
	if (c->display != EGL_NO_DISPLAY) {
		eglTerminate(c->display);
		c->display = EGL_NO_DISPLAY;
	}
	// eglReleaseThread();

	G_OBJECT_CLASS(rf_converter_parent_class)->finalize(o);
}

static void rf_converter_init(RfConverter *c)
{
	c->width = RF_DEFAULT_HEIGHT;
	c->height = RF_DEFAULT_HEIGHT;
	c->program = 0;
	c->vertex_array = 0;
	c->framebuffer = 0;
	c->texture = 0;

	_init_egl(c);
	_init_gl(c);
}

static void rf_converter_class_init(RfConverterClass *c_class)
{
	GObjectClass *o_class = G_OBJECT_CLASS(c_class);

	o_class->finalize = _finalize;
}

RfConverter *rf_converter_new(void)
{
	return g_object_new(RF_TYPE_CONVERTER, NULL);
}

static inline void _append_attrib(GArray *a, EGLAttrib k, EGLAttrib v)
{
	g_array_append_val(a, k);
	g_array_append_val(a, v);
}

unsigned char *rf_converter_convert(RfConverter *c, const RfBuffer *b,
				    unsigned int width, unsigned int height)
{
	EGLAttrib fd_keys[RF_MAX_PLANES] = { EGL_DMA_BUF_PLANE0_FD_EXT,
					     EGL_DMA_BUF_PLANE1_FD_EXT,
					     EGL_DMA_BUF_PLANE2_FD_EXT,
					     EGL_DMA_BUF_PLANE3_FD_EXT };
	EGLAttrib offset_keys[RF_MAX_PLANES] = {
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT
	};
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
			_append_attrib(image_attribs, modlo_keys[i],
				       (b->md.modifier & 0xFFFFFFFF));
			_append_attrib(image_attribs, modhi_keys[i],
				       (b->md.modifier >> 32));
		}
	}
	EGLAttrib key = EGL_NONE;
	g_array_append_val(image_attribs, key);

	// EGL_NO_CONTEXT must be used here according to the docs.
	//
	// See <https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt>.
	EGLImage image = eglCreateImage(c->display, EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT,
					(EGLClientBuffer)NULL,
					(EGLAttrib *)image_attribs->data);

	g_array_free(image_attribs, true);

	if (image == EGL_NO_IMAGE) {
		g_error("Cannot create image: %d.", eglGetError());
	}

	if (!eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    c->context)) {
		g_error("Cannot make current: %d.", eglGetError());
		// exit(EXIT_FAILURE);
	}

	width = width > 0 ? width : b->md.width;
	height = height > 0 ? height : b->md.height;
	if (c->width != width || c->height != height) {
		g_debug("Target size changed, generating new texture for %dx%d.",
			width, height);
		c->width = width;
		c->height = height;
		_gen_texture(c);
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
	g_debug("glEGLImageTargetTexture2DOES: %#x", glGetError());

	// This would be the easiest way if I can use full OpenGL. However,
	// libGL will pulls libGLX, I don't want that. With OpenGL ES we don't
	// have `glGetTexImage()`, so we have to use framebuffers.
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf);

	glBindFramebuffer(GL_FRAMEBUFFER, c->framebuffer);
	glViewport(0, 0, c->width, c->height);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(c->program);
	glUniform1i(glGetUniformLocation(c->program, "image"), 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(c->vertex_array);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
	g_debug("glDrawElements: %#x", glGetError());
	glBindVertexArray(0);
	glUseProgram(0);
	unsigned char *buf =
		g_malloc(RF_BYTES_PER_PIXEL * c->width * c->height);
	glPixelStorei(GL_PACK_ALIGNMENT, RF_BYTES_PER_PIXEL);
	// OpenGL ES only ensures `GL_RGBA` and `GL_RGB`, `GL_BGRA` is optional.
	// But luckily LibVNCServer accepts RGBA by default.
	glReadPixels(0, 0, c->width, c->height, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	g_debug("glReadPixels: %#x", glGetError());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &texture);

	eglDestroyImage(c->display, image);

	return buf;
}
