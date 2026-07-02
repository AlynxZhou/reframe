#include <assert.h>
#include <glib.h>
#include <unistd.h>

#include "rf-config.h"

static char *write_temp_config(const char *contents)
{
	g_autofree char *path = NULL;
	g_autoptr(GError) error = NULL;
	int fd = g_file_open_tmp("reframe-config-XXXXXX", &path, &error);

	assert(fd >= 0);
	close(fd);
	assert(g_file_set_contents(path, contents, -1, &error));
	return g_steal_pointer(&path);
}

static void test_rdp_max_fps_default(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");

	assert(rf_config_get_rdp_max_fps(config) == 30);
}

static void test_rdp_max_fps_custom(void)
{
	g_autofree char *path = write_temp_config("[rdp]\nmax-fps=15\n");
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_max_fps(config) == 15);
	unlink(path);
}

static void test_rdp_max_fps_zero_is_unlimited(void)
{
	g_autofree char *path = write_temp_config("[rdp]\nmax-fps=0\n");
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_max_fps(config) == 0);
	unlink(path);
}

static void test_rdp_avc_encoder_default(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");
	g_autofree char *encoder = rf_config_get_rdp_avc_encoder(config);

	assert(g_strcmp0(encoder, "auto") == 0);
}

static void test_rdp_avc_encoder_custom(void)
{
	g_autofree char *path = write_temp_config("[rdp]\navc-encoder=h264_vaapi\n");
	g_autoptr(RfConfig) config = rf_config_new(path);
	g_autofree char *encoder = rf_config_get_rdp_avc_encoder(config);

	assert(g_strcmp0(encoder, "h264_vaapi") == 0);
	unlink(path);
}

static void test_rdp_video_quality_default_auto(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");

	assert(rf_config_get_rdp_video_quality(config) == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO);
	assert(rf_config_get_rdp_video_quality_max(config) == 3);
}

static void test_rdp_video_quality_custom(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\nvideo-quality=2\nvideo-quality-max=1\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_video_quality(config) == 2);
	assert(rf_config_get_rdp_video_quality_max(config) == 1);
	unlink(path);
}

static void test_rdp_video_quality_invalid_uses_defaults(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\nvideo-quality=fast\nvideo-quality-max=99\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_video_quality(config) == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO);
	assert(rf_config_get_rdp_video_quality_max(config) == 3);
	unlink(path);
}

static void test_rdp_target_bandwidth_default(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");

	assert(rf_config_get_rdp_target_bandwidth_mbps(config) == 20);
}

static void test_rdp_target_bandwidth_custom(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\ntarget-bandwidth-mbps=12\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_target_bandwidth_mbps(config) == 12);
	unlink(path);
}

static void test_rdp_target_bandwidth_invalid_uses_default(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\ntarget-bandwidth-mbps=0\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_target_bandwidth_mbps(config) == 20);
	unlink(path);
}

int main(void)
{
	test_rdp_max_fps_default();
	test_rdp_max_fps_custom();
	test_rdp_max_fps_zero_is_unlimited();
	test_rdp_avc_encoder_default();
	test_rdp_avc_encoder_custom();
	test_rdp_video_quality_default_auto();
	test_rdp_video_quality_custom();
	test_rdp_video_quality_invalid_uses_defaults();
	test_rdp_target_bandwidth_default();
	test_rdp_target_bandwidth_custom();
	test_rdp_target_bandwidth_invalid_uses_default();
	return 0;
}
