#ifndef __RF_RDP_AUDIO_PIPEWIRE_H__
#define __RF_RDP_AUDIO_PIPEWIRE_H__

#include <stdbool.h>

bool rf_rdp_audio_pipewire_run(
	const char *socket_path,
	const char *target,
	unsigned int sample_rate,
	unsigned int channels,
	unsigned int frame_ms
);

#endif
