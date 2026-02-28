#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>

#define MVPRINT g_debug
#include "mvmath.h"

#include "rf-common.h"
#include "rf-converter.h"

#define GL_MAX_BUFFERS 3

struct _RfConverter {
	GObject parent_instance;
	RfConfig *config;
	char *card_path;
	unsigned int gles_major;
	EGLDisplay display;
	EGLContext context;
	GByteArray *curr;
	GByteArray *prev;
	unsigned int width;
	unsigned int height;
	unsigned int prev_width;
	unsigned int prev_height;
	unsigned int damage_width;
	unsigned int damage_height;
	unsigned int buffers[GL_MAX_BUFFERS];
	unsigned int draw_vertex_array;
	unsigned int damage_vertex_array;
	unsigned int draw_program;
	unsigned int damage_program;
	unsigned int draw_framebuffer;
	unsigned int damage_framebuffer;
	unsigned int curr_texture;
	unsigned int prev_texture;
	unsigned int damage_texture;
	unsigned int tile_size;
	unsigned int rotation;
	enum rf_damage_type damage_type;
	bool running;
};
G_DEFINE_TYPE(RfConverter, rf_converter, G_TYPE_OBJECT)

static EGLDisplay get_egl_display_from_drm_card(const char *card_path)
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

	for (int i = 0; i < num_devices; ++i) {
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

static int setup_egl(RfConverter *this)
{
	EGLint major;
	EGLint minor;
	EGLint count;
	EGLint n;
	EGLint size;
	EGLConfig config = NULL;
	g_autofree EGLConfig *configs = NULL;
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

	this->display = get_egl_display_from_drm_card(this->card_path);
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

	return 0;
}

static void clean_egl(RfConverter *this)
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
static void gl_message(
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

static unsigned int make_shader(GLenum type, const char *s)
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

static unsigned int make_program(const char *vs, const char *fs)
{
	unsigned int v = make_shader(GL_VERTEX_SHADER, vs);
	unsigned int f = make_shader(GL_FRAGMENT_SHADER, fs);
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

static void bind_buffers(RfConverter *this, unsigned int program)
{
	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[0]);
	glVertexAttribPointer(
		glGetAttribLocation(program, "in_position"),
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(float),
		0
	);
	glEnableVertexAttribArray(glGetAttribLocation(program, "in_position"));

	glBindBuffer(GL_ARRAY_BUFFER, this->buffers[1]);
	glVertexAttribPointer(
		glGetAttribLocation(program, "in_coordinate"),
		2,
		GL_FLOAT,
		GL_FALSE,
		2 * sizeof(float),
		0
	);
	glEnableVertexAttribArray(glGetAttribLocation(program, "in_coordinate"));

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->buffers[2]);
}

static void unbind_buffers(RfConverter *this, unsigned int program)
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(glGetAttribLocation(program, "in_position"));

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(
		glGetAttribLocation(program, "in_coordinate")
	);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static int setup_gl(RfConverter *this)
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
	glDebugMessageCallback(gl_message, NULL);
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
		const char draw_fs[] =
			"#version 300 es\n"
			"#ifdef GL_OES_EGL_image_external_essl3\n"
			"#extension GL_OES_EGL_image_external_essl3 : require\n"
			"#else\n"
			"#extension GL_OES_EGL_image_external : require\n"
			"#endif\n"
			"precision highp float;\n"
			"uniform mat4 crop;\n"
			"uniform samplerExternalOES image;\n"
			"in vec2 pass_coordinate;\n"
			"out vec4 out_color;\n"
			"void main() {\n"
			"	vec4 out_coordinate = crop * vec4(pass_coordinate, 0.0f, 1.0f);\n"
			"	out_color = texture(image, out_coordinate.xy);\n"
			"}\n";
		this->draw_program = make_program(vs, draw_fs);
		const char damage_fs[] =
			"#version 300 es\n"
			"precision highp float;\n"
			"uniform sampler2D curr;\n"
			"uniform sampler2D prev;\n"
			"in vec2 pass_coordinate;\n"
			"out vec4 out_color;\n"
			"void main() {\n"
			"	vec4 currp = texture(curr, pass_coordinate);\n"
			"	vec4 prevp = texture(prev, pass_coordinate);\n"
			"	if (any(notEqual(currp, prevp)))\n"
			"		out_color = vec4(1.0f, 1.0f, 1.0f, 1.0f);\n"
			"	else\n"
			"		out_color = vec4(0.0f, 0.0f, 0.0f, 1.0f);\n"
			"}\n";
		this->damage_program = make_program(vs, damage_fs);
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
		const char draw_fs[] =
			"#version 100\n"
			"#extension GL_OES_EGL_image_external : require\n"
			"precision highp float;\n"
			"uniform mat4 crop;\n"
			"uniform samplerExternalOES image;\n"
			"varying vec2 pass_coordinate;\n"
			"void main() {\n"
			"	vec4 out_coordinate = crop * vec4(pass_coordinate, 0.0, 1.0);\n"
			"	gl_FragColor = texture2D(image, out_coordinate.xy);\n"
			"}\n";
		this->draw_program = make_program(vs, draw_fs);
		const char damage_fs[] =
			"#version 100\n"
			"precision highp float;\n"
			"uniform sampler2D curr;\n"
			"uniform sampler2D prev;\n"
			"varying vec2 pass_coordinate;\n"
			"void main() {\n"
			"	vec4 currp = texture2D(curr, pass_coordinate);\n"
			"	vec4 prevp = texture2D(prev, pass_coordinate);\n"
			"	if (any(notEqual(currp, prevp)))\n"
			"		gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
			"	else\n"
			"		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
			"}\n";
		this->damage_program = make_program(vs, damage_fs);
	}
	if (this->draw_program == 0)
		return -5;
	if (this->damage_program == 0)
		return -6;

	glUseProgram(this->draw_program);
	// Use texture 0 for this sampler. This only needs to be done once.
	glUniform1i(glGetUniformLocation(this->draw_program, "image"), 0);
	glUseProgram(0);

	glUseProgram(this->damage_program);
	glUniform1i(glGetUniformLocation(this->damage_program, "curr"), 0);
	glUniform1i(glGetUniformLocation(this->damage_program, "prev"), 1);
	glUseProgram(0);

	glGenBuffers(GL_MAX_BUFFERS, this->buffers);

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
	const float coordinates[] = {
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
		glGenVertexArrays(1, &this->draw_vertex_array);
		glBindVertexArray(this->draw_vertex_array);
		bind_buffers(this, this->draw_program);
		glBindVertexArray(0);

		glGenVertexArrays(1, &this->damage_vertex_array);
		glBindVertexArray(this->damage_vertex_array);
		bind_buffers(this, this->damage_program);
		glBindVertexArray(0);
	}

	glGenFramebuffers(1, &this->draw_framebuffer);
	glGenFramebuffers(1, &this->damage_framebuffer);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	return 0;
}

static void clean_gl(RfConverter *this)
{
	if (this->buffers[0] != 0) {
		glDeleteBuffers(GL_MAX_BUFFERS, this->buffers);
		this->buffers[0] = 0;
		this->buffers[1] = 0;
		this->buffers[2] = 0;
	}
	if (this->draw_vertex_array != 0) {
		glDeleteVertexArrays(1, &this->draw_vertex_array);
		this->draw_vertex_array = 0;
	}
	if (this->damage_vertex_array != 0) {
		glDeleteVertexArrays(1, &this->damage_vertex_array);
		this->damage_vertex_array = 0;
	}
	if (this->draw_program != 0) {
		glDeleteProgram(this->draw_program);
		this->draw_program = 0;
	}
	if (this->damage_program != 0) {
		glDeleteProgram(this->damage_program);
		this->damage_program = 0;
	}
	if (this->draw_framebuffer != 0) {
		glDeleteFramebuffers(1, &this->draw_framebuffer);
		this->draw_framebuffer = 0;
	}
	if (this->damage_framebuffer != 0) {
		glDeleteFramebuffers(1, &this->damage_framebuffer);
		this->damage_framebuffer = 0;
	}
	if (this->curr_texture != 0) {
		glDeleteTextures(1, &this->curr_texture);
		this->curr_texture = 0;
	}
	if (this->prev_texture != 0) {
		glDeleteTextures(1, &this->prev_texture);
		this->prev_texture = 0;
	}
	if (this->damage_texture != 0) {
		glDeleteTextures(1, &this->damage_texture);
		this->damage_texture = 0;
	}
}

static void finalize(GObject *o)
{
	RfConverter *this = RF_CONVERTER(o);

	rf_converter_stop(this);

	G_OBJECT_CLASS(rf_converter_parent_class)->finalize(o);
}

static void rf_converter_class_init(RfConverterClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = finalize;
}

static void rf_converter_init(RfConverter *this)
{
	this->config = NULL;
	this->card_path = NULL;
	this->gles_major = 3;
	this->display = EGL_NO_DISPLAY;
	this->context = EGL_NO_CONTEXT;
	this->curr = NULL;
	this->prev = NULL;
	this->width = 0;
	this->height = 0;
	this->prev_width = 0;
	this->prev_height = 0;
	this->damage_width = 0;
	this->damage_height = 0;
	this->buffers[0] = 0;
	this->buffers[1] = 0;
	this->buffers[2] = 0;
	this->draw_vertex_array = 0;
	this->damage_vertex_array = 0;
	this->draw_program = 0;
	this->damage_program = 0;
	this->draw_framebuffer = 0;
	this->damage_framebuffer = 0;
	this->curr_texture = 0;
	this->prev_texture = 0;
	this->damage_texture = 0;
	this->tile_size = 4;
	this->rotation = 0;
	this->damage_type = RF_DAMAGE_TYPE_CPU;
	this->running = false;
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
	this->damage_type = rf_config_get_damage(this->config);
	switch (this->damage_type) {
	case RF_DAMAGE_TYPE_CPU:
		g_message(
			"Frame: Damage region detection implementation is CPU."
		);
		break;
	case RF_DAMAGE_TYPE_GPU:
		g_message(
			"Frame: Damage region detection implementation is GPU."
		);
		break;
	default:
		g_message("Frame: No damage region detection implementation.");
		break;
	}
	this->width = 0;
	this->height = 0;
	this->prev_width = 0;
	this->prev_height = 0;
	int ret = 0;
	ret = setup_egl(this);
	if (ret < 0)
		goto out;
	ret = setup_gl(this);
	if (ret < 0)
		goto out;

	this->running = true;

out:
	if (ret < 0) {
		clean_gl(this);
		clean_egl(this);
		g_clear_pointer(&this->card_path, g_free);
	}
	return ret;
}

bool rf_converter_is_running(RfConverter *this)
{
	g_return_val_if_fail(RF_IS_CONVERTER(this), false);

	return this->running;
}

void rf_converter_stop(RfConverter *this)
{
	g_return_if_fail(RF_IS_CONVERTER(this));

	if (!this->running)
		return;

	this->running = false;

	g_clear_pointer(&this->curr, g_byte_array_unref);
	g_clear_pointer(&this->prev, g_byte_array_unref);
	g_clear_pointer(&this->card_path, g_free);
	clean_gl(this);
	clean_egl(this);
}

// We downscale texture into tiles on GPU, because we still need to scan the
// result on CPU to get damage region, and per-pixel scanning is too heavy.
// Because we actually compare the linear average color of a tile in shader,
// larger tile size reduces the accuracy and causes artifacts. To increase
// accuracy we generate mipmaps for textures and sampling the nearest mipmap,
// then we need to ensure that tile size is power-of-2 to match mipmap.
//
// For CPU damage region detection we could safely choose a relative larger value
// as tile size. 16 is a balanced value between comparing and transfering.
static void update_damage_size(RfConverter *this)
{
	if (this->damage_type == RF_DAMAGE_TYPE_GPU) {
		if (this->width >= 1280 && this->height >= 720)
			this->tile_size = 4;
		else
			this->tile_size = 2;
	} else {
		this->tile_size = 16;
	}

	g_debug("GL: Set tile size of damage to %u.", this->tile_size);

	this->damage_width =
		(this->width + this->tile_size - 1) / this->tile_size;
	this->damage_height =
		(this->height + this->tile_size - 1) / this->tile_size;
}

static inline void set_texture_parameters(GLenum target, GLint min_filter)
{
	// Setting sampling filter to scaling texture automatically.
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, min_filter);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static void gen_textures(RfConverter *this)
{
	g_debug("GL: Generating new draw textures for width %u and height %u.",
		this->width,
		this->height);

	if (this->curr_texture != 0)
		glDeleteTextures(1, &this->curr_texture);
	glGenTextures(1, &this->curr_texture);
	glBindTexture(GL_TEXTURE_2D, this->curr_texture);
	set_texture_parameters(GL_TEXTURE_2D, GL_LINEAR_MIPMAP_NEAREST);
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
	glBindTexture(GL_TEXTURE_2D, 0);

	if (this->prev_texture != 0)
		glDeleteTextures(1, &this->prev_texture);
	glGenTextures(1, &this->prev_texture);
	glBindTexture(GL_TEXTURE_2D, this->prev_texture);
	set_texture_parameters(GL_TEXTURE_2D, GL_LINEAR_MIPMAP_NEAREST);
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
	glBindTexture(GL_TEXTURE_2D, 0);

	g_debug("GL: Generating new damage texture for width %u and height %u.",
		this->damage_width,
		this->damage_height);

	if (this->damage_texture != 0)
		glDeleteTextures(1, &this->damage_texture);
	glGenTextures(1, &this->damage_texture);
	glBindTexture(GL_TEXTURE_2D, this->damage_texture);
	// We won't sample this texture so this is useless.
	// set_texture_parameters(GL_TEXTURE_2D, GL_LINEAR_MIPMAP_NEAREST);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RGBA,
		this->damage_width,
		this->damage_height,
		0,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		NULL
	);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void gen_buffers(RfConverter *this)
{
	g_debug("GL: Generate new buffers for width %u and height %u.",
		this->width,
		this->height);

	const unsigned int size =
		RF_BYTES_PER_PIXEL * this->width * this->height;

	g_clear_pointer(&this->curr, g_byte_array_unref);
	this->curr = g_byte_array_sized_new(size);

	g_clear_pointer(&this->prev, g_byte_array_unref);
	this->prev = g_byte_array_sized_new(size);
	// Clear prev buffer so we will get a full update.
	memset(this->prev->data, 0, size);
}

static inline void append_attrib(GArray *a, EGLAttrib k, EGLAttrib v)
{
	g_array_append_val(a, k);
	g_array_append_val(a, v);
}

static EGLImage make_image(EGLDisplay *display, const struct rf_buffer *b)
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
	append_attrib(image_attribs, EGL_WIDTH, b->md.fb_width);
	append_attrib(image_attribs, EGL_HEIGHT, b->md.fb_height);
	append_attrib(image_attribs, EGL_LINUX_DRM_FOURCC_EXT, b->md.fourcc);
	for (unsigned int i = 0; i < b->md.length; ++i) {
		append_attrib(image_attribs, fd_keys[i], b->fds[i]);
		append_attrib(image_attribs, offset_keys[i], b->md.offsets[i]);
		append_attrib(image_attribs, pitch_keys[i], b->md.pitches[i]);
		if (b->md.modifier != DRM_FORMAT_MOD_INVALID) {
			append_attrib(
				image_attribs,
				modlo_keys[i],
				(b->md.modifier & 0xFFFFFFFF)
			);
			append_attrib(
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

static void draw_begin(RfConverter *this)
{
	glBindFramebuffer(GL_FRAMEBUFFER, this->draw_framebuffer);
	// We always rebind texture to framebuffer because we swap current and
	// previous textures.
	glFramebufferTexture2D(
		GL_FRAMEBUFFER,
		GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D,
		this->curr_texture,
		0
	);
	glViewport(0, 0, this->width, this->height);

	glUseProgram(this->draw_program);
	if (this->gles_major >= 3)
		glBindVertexArray(this->draw_vertex_array);
	else
		bind_buffers(this, this->draw_program);
}

static void draw_rect(
	RfConverter *this,
	EGLImage image,
	// Texture coordinates.
	uint32_t sx,
	uint32_t sy,
	uint32_t sw,
	uint32_t sh,
	uint32_t texture_width,
	uint32_t texture_height,
	// Vertex coordinates.
	int32_t x,
	int32_t y,
	uint32_t w,
	uint32_t h,
	int32_t z,
	uint32_t canvas_width,
	uint32_t canvas_height
)
{
	unsigned int texture;
	glGenTextures(1, &texture);
	glActiveTexture(GL_TEXTURE0);
	// While `GL_TEXTURE_2D` does work for most cases, it won't work with
	// NVIDIA and linear modifier (which is used by TTY), we have to use
	// `GL_TEXTURE_EXTERNAL_OES`.
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
	// `GL_TEXTURE_EXTERNAL_OES` does not support mipmap.
	set_texture_parameters(GL_TEXTURE_EXTERNAL_OES, GL_LINEAR);
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
		m4ortho(0.0f, canvas_width, canvas_height, 0.0f, 0.1f, 100.0f);
	mat4 mvp = m4multiply(projection, m4multiply(view, model));
	// Rotating monitor is just the same as rotating the whole world.
	if (this->rotation % 360 != 0)
		mvp = m4multiply(
			m4rotate(
				v3s(0.0f, 0.0f, -1.0f), sradians(this->rotation)
			),
			mvp
		);

	mat4 crop = m4identity();
	if (sx != 0 || sy != 0 || sw != texture_width || sh != texture_height)
		crop = m4multiply(
			m4translate(
				v3s((float)sx / texture_width,
				    (float)sy / texture_height,
				    0.0f)
			),
			m4scale(
				v3s((float)sw / texture_width,
				    (float)sh / texture_height,
				    1.0f)
			)
		);

	glUniformMatrix4fv(
		glGetUniformLocation(this->draw_program, "mvp"),
		1,
		false,
		MARRAY(mvp)
	);
	glUniformMatrix4fv(
		glGetUniformLocation(this->draw_program, "crop"),
		1,
		false,
		MARRAY(crop)
	);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, 0);
	// g_debug("glDrawElements: %#x", glGetError());

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glDeleteTextures(1, &texture);
}

static void draw_buffer(
	RfConverter *this,
	const struct rf_buffer *b,
	int32_t z,
	uint32_t frame_width,
	uint32_t frame_height
)
{
	EGLImage image = make_image(this->display, b);
	if (image == EGL_NO_IMAGE) {
		g_warning("EGL: Failed to create image: %d.", eglGetError());
		return;
	}
	draw_rect(
		this,
		image,
		b->md.src_x,
		b->md.src_y,
		b->md.src_w,
		b->md.src_h,
		b->md.fb_width,
		b->md.fb_height,
		b->md.crtc_x,
		b->md.crtc_y,
		b->md.crtc_w,
		b->md.crtc_h,
		z,
		frame_width,
		frame_height
	);
	eglDestroyImage(this->display, image);
}

static void draw_end(RfConverter *this)
{
	if (this->gles_major >= 3)
		glBindVertexArray(0);
	else
		unbind_buffers(this, this->draw_program);
	glUseProgram(0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int
convert_buffers(RfConverter *this, size_t length, const struct rf_buffer *bufs)
{
	int res = 0;

	draw_begin(this);

	const struct rf_buffer *primary = &bufs[0];
	// Monitor size should be CRTC size.
	const uint32_t frame_width = primary->md.crtc_width;
	const uint32_t frame_height = primary->md.crtc_height;

	// When we cover the whole frame, it should be OK that we don't clear
	// those buffers to improve performance.
	if (primary->md.crtc_x > 0 || primary->md.crtc_y > 0 ||
	    primary->md.crtc_x + primary->md.crtc_w < frame_width ||
	    primary->md.crtc_y + primary->md.crtc_h < frame_height)
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	for (size_t i = 0; i < length; ++i)
		draw_buffer(
			this, &bufs[i], length - i, frame_width, frame_height
		);

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
		this->curr->data
	);
	if (glGetError() != GL_NO_ERROR)
		res = -1;

	draw_end(this);
	return res;
}

static void damage_full(RfConverter *this, struct rf_rect *damage)
{
	damage->x = 0;
	damage->y = 0;
	damage->w = this->width;
	damage->h = this->height;
}

static void damage_begin(RfConverter *this)
{
	glBindFramebuffer(GL_FRAMEBUFFER, this->damage_framebuffer);
	glFramebufferTexture2D(
		GL_FRAMEBUFFER,
		GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D,
		this->damage_texture,
		0
	);
	glViewport(0, 0, this->damage_width, this->damage_height);

	glUseProgram(this->damage_program);
	if (this->gles_major >= 3)
		glBindVertexArray(this->damage_vertex_array);
	else
		bind_buffers(this, this->damage_program);
}

static void damage_rect(
	RfConverter *this,
	int32_t x,
	int32_t y,
	uint32_t w,
	uint32_t h,
	int32_t z,
	uint32_t canvas_width,
	uint32_t canvas_height
)
{
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, this->curr_texture);
	glGenerateMipmap(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, this->prev_texture);
	glGenerateMipmap(GL_TEXTURE_2D);

	mat4 model =
		m4multiply(m4translate(v3s(x, y, z)), m4scale(v3s(w, h, 1.0f)));
	mat4 view = m4camera(
		v3s(0.0f, 0.0f, 0.0f),
		v3s(0.0f, 0.0f, 1.0f),
		v3s(0.0f, 1.0f, 0.0f)
	);
	mat4 projection =
		m4ortho(0.0f, canvas_width, canvas_height, 0.0f, 0.1f, 100.0f);
	mat4 mvp = m4multiply(projection, m4multiply(view, model));

	glUniformMatrix4fv(
		glGetUniformLocation(this->damage_program, "mvp"),
		1,
		false,
		MARRAY(mvp)
	);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void damage_end(RfConverter *this)
{
	if (this->gles_major >= 3)
		glBindVertexArray(0);
	else
		unbind_buffers(this, this->damage_program);
	glUseProgram(0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void detect_damage_gpu(RfConverter *this, struct rf_rect *damage)
{
	damage_begin(this);

	// We always redraw the whole damage framebuffer so this is useless.
	// glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Always fullfill the whole canvas.
	damage_rect(
		this,
		0,
		0,
		this->damage_width,
		this->damage_height,
		1,
		this->damage_width,
		this->damage_height
	);

	g_autofree uint8_t *damage_buffer = g_malloc0_n(
		this->damage_width * this->damage_height * RF_BYTES_PER_PIXEL,
		sizeof(*damage_buffer)
	);
	glPixelStorei(GL_PACK_ALIGNMENT, RF_BYTES_PER_PIXEL);
	glReadPixels(
		0,
		0,
		this->damage_width,
		this->damage_height,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		damage_buffer
	);
	if (glGetError() != GL_NO_ERROR) {
		g_debug("Frame: Reading damage error, return full damage.");
		damage_full(this, damage);
		goto out;
	}

	unsigned int x1 = this->width;
	unsigned int y1 = this->height;
	unsigned int x2 = 0;
	unsigned int y2 = 0;

	const size_t stride = this->damage_width * RF_BYTES_PER_PIXEL;
	for (unsigned int yt = 0; yt < this->damage_height; ++yt) {
		for (unsigned int xt = 0; xt < this->damage_width; ++xt) {
			const size_t offset =
				yt * stride + xt * RF_BYTES_PER_PIXEL;
			// Checking only the red channel is enough.
			if (damage_buffer[offset] > 0) {
				const unsigned int x = xt * this->tile_size;
				const unsigned int y = yt * this->tile_size;
				const unsigned int w =
					MIN(this->tile_size, this->width - x);
				const unsigned int h =
					MIN(this->tile_size, this->height - y);

				x1 = MIN(x1, x);
				y1 = MIN(y1, y);
				x2 = MAX(x2, x + w);
				y2 = MAX(y2, y + h);
			}
		}
	}

	if (x1 < x2 && y1 < y2) {
		damage->x = x1;
		damage->y = y1;
		damage->w = x2 - x1;
		damage->h = y2 - y1;
	} else {
		damage->x = 0;
		damage->y = 0;
		damage->w = 0;
		damage->h = 0;
	}

out:
	damage_end(this);
}

// This is a naive damage region detection but works fairly enough for us.
static void detect_damage_cpu(RfConverter *this, struct rf_rect *damage)
{
	unsigned int x1 = this->width;
	unsigned int y1 = this->height;
	unsigned int x2 = 0;
	unsigned int y2 = 0;

	// Optimization: We don't compare by square tiles, but compare by rows,
	// so we are accessing continuous memory.
	//
	// This will increase network bandwidth compared with square tiles based
	// detection, but on the earth I am doing what VNC/RDP encoders should
	// concern (but they does not), so please don't be too harsh on me. IMO
	// you should avoid using remote desktop with mobile data hotspot.
	const uint8_t *new = this->curr->data;
	const uint8_t *old = this->prev->data;
	const size_t stride = this->width * RF_BYTES_PER_PIXEL;
	const unsigned int x = 0;
	const unsigned int w = this->width;
	for (unsigned int y = 0; y < this->height; y += this->tile_size) {
		const unsigned int h = MIN(this->tile_size, this->height - y);
		const size_t offset = y * stride + x * RF_BYTES_PER_PIXEL;
		const size_t size = w * h * RF_BYTES_PER_PIXEL;
		if (memcmp(new + offset, old + offset, size) != 0) {
			x1 = MIN(x1, x);
			y1 = MIN(y1, y);
			x2 = MAX(x2, x + w);
			y2 = MAX(y2, y + h);
		}
	}

	if (x1 < x2 && y1 < y2) {
		damage->x = x1;
		damage->y = y1;
		damage->w = x2 - x1;
		damage->h = y2 - y1;
	} else {
		damage->x = 0;
		damage->y = 0;
		damage->w = 0;
		damage->h = 0;
	}
}

static void detect_damage(RfConverter *this, struct rf_rect *damage)
{
	if (this->damage_type == RF_DAMAGE_TYPE_GPU) {
		detect_damage_gpu(this, damage);
		unsigned int swap_texture = this->curr_texture;
		this->curr_texture = this->prev_texture;
		this->prev_texture = swap_texture;
	} else if (this->damage_type == RF_DAMAGE_TYPE_CPU) {
		detect_damage_cpu(this, damage);
		memcpy(this->prev->data,
		       this->curr->data,
		       RF_BYTES_PER_PIXEL * this->width * this->height);
	} else {
		damage_full(this, damage);
	}
	g_debug("Frame: Got buffer damage: x %u, y %u, width %u, height %u.",
		damage->x,
		damage->y,
		damage->w,
		damage->h);
}

GByteArray *rf_converter_convert(
	RfConverter *this,
	size_t length,
	const struct rf_buffer *bufs,
	unsigned int width,
	unsigned int height,
	struct rf_rect *damage
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

#ifdef __DEBUG__
	const int64_t begin = g_get_monotonic_time();
#endif

	if (this->width != width || this->height != height) {
		this->width = width;
		this->height = height;
		update_damage_size(this);
		gen_textures(this);
		gen_buffers(this);
	}

	int res = convert_buffers(this, length, bufs);
	if (res >= 0 && damage != NULL)
		detect_damage(this, damage);

#ifdef __DEBUG__
	const int64_t end = g_get_monotonic_time();
	g_debug("GL: Converted frame in %ldms.", (end - begin) / 1000);
#endif

	if (res < 0)
		return NULL;

	if (damage != NULL && damage->w == 0 && damage->h == 0) {
		g_debug("Frame: Empty damage, return empty buffer.");
		return NULL;
	}

	return this->curr;
}
