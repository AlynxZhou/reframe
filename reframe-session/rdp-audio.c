#include <locale.h>
#include <string.h>

#include <gio/gio.h>

#include "rf-rdp-audio-pipewire.h"

int main(int argc, char **argv)
{
	g_autofree char *socket_path = NULL;
	g_autofree char *socket_dir = NULL;
	g_autoptr(GError) error = NULL;
	unsigned int sample_rate = 48000;
	unsigned int channels = 2;
	unsigned int frame_ms = 20;
	GOptionEntry options[] = {
		{ "socket", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &socket_path, "RDP audio socket path.", "SOCKET" },
		{ "socket-dir", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &socket_dir, "RDP audio socket dir to communicate.", "DIR" },
		{ "sample-rate", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
		  &sample_rate, "PCM sample rate.", "HZ" },
		{ "channels", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
		  &channels, "PCM channel count.", "N" },
		{ "frame-ms", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
		  &frame_ms, "PCM frame size in milliseconds.", "MS" },
		{ NULL }
	};
	g_autoptr(GOptionContext) context =
		g_option_context_new(" - ReFrame RDP Audio Helper");

	setlocale(LC_ALL, "");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		return 1;
	}
	if (socket_path == NULL) {
		if (socket_dir == NULL)
			socket_dir = g_strdup("/tmp/reframe-rdp-audio");
		const size_t length = strlen(socket_dir);
		if (length > 0 && socket_dir[length - 1] == '/')
			socket_dir[length - 1] = '\0';

		g_autoptr(GDir) dir = g_dir_open(socket_dir, 0, NULL);
		if (dir != NULL) {
			const char *name = g_dir_read_name(dir);
			if (name != NULL)
				socket_path = g_build_filename(socket_dir, name, NULL);
		}
		if (socket_path == NULL)
			socket_path = g_build_filename(
				socket_dir,
				"reframe-rdp-audio.sock",
				NULL
			);
	}

	return rf_rdp_audio_pipewire_run(
		socket_path,
		sample_rate,
		channels,
		frame_ms
	) ? 0 : 1;
}
