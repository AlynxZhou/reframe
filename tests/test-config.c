#include <assert.h>
#include <glib.h>
#include <string.h>
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

static void test_fps_default(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");

	assert(rf_config_get_fps(config) == 30);
}

static void test_fps_custom(void)
{
	g_autofree char *path = write_temp_config("[reframe]\nfps=60\n");
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_fps(config) == 60);
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

static void test_rdp_audio_defaults(void)
{
	g_autoptr(RfConfig) config = rf_config_new("/nonexistent/reframe.conf");

	assert(!rf_config_get_rdp_audio(config));
	assert(rf_config_get_rdp_audio_sample_rate(config) == 48000);
	assert(rf_config_get_rdp_audio_channels(config) == 2);
	assert(rf_config_get_rdp_audio_frame_ms(config) == 20);
	assert(strcmp(rf_config_get_rdp_audio_codec(config), "auto") == 0);
}

static void test_rdp_audio_config_values(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\n"
		"audio=true\n"
		"audio-sample-rate=44100\n"
		"audio-channels=1\n"
		"audio-frame-ms=10\n"
		"audio-codec=adpcm\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_audio(config));
	assert(rf_config_get_rdp_audio_sample_rate(config) == 44100);
	assert(rf_config_get_rdp_audio_channels(config) == 1);
	assert(rf_config_get_rdp_audio_frame_ms(config) == 10);
	assert(strcmp(rf_config_get_rdp_audio_codec(config), "adpcm") == 0);
	unlink(path);
}

static void test_rdp_audio_invalid_uses_defaults(void)
{
	g_autofree char *path = write_temp_config(
		"[rdp]\n"
		"audio-sample-rate=32000\n"
		"audio-channels=8\n"
		"audio-frame-ms=5\n"
		"audio-codec=mp3\n"
	);
	g_autoptr(RfConfig) config = rf_config_new(path);

	assert(rf_config_get_rdp_audio_sample_rate(config) == 48000);
	assert(rf_config_get_rdp_audio_channels(config) == 2);
	assert(rf_config_get_rdp_audio_frame_ms(config) == 20);
	assert(strcmp(rf_config_get_rdp_audio_codec(config), "auto") == 0);
	unlink(path);
}

int main(void)
{
	test_fps_default();
	test_fps_custom();
	test_rdp_avc_encoder_default();
	test_rdp_avc_encoder_custom();
	test_rdp_video_quality_default_auto();
	test_rdp_video_quality_custom();
	test_rdp_video_quality_invalid_uses_defaults();
	test_rdp_target_bandwidth_default();
	test_rdp_target_bandwidth_custom();
	test_rdp_target_bandwidth_invalid_uses_default();
	test_rdp_audio_defaults();
	test_rdp_audio_config_values();
	test_rdp_audio_invalid_uses_defaults();
	return 0;
}
