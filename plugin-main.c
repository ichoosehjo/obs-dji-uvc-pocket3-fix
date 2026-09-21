/*
 * obs-dji-uvc — module entry point.
 * License: GPL-2.0-or-later
 */
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-dji-uvc", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "DJI Osmo Pocket 3/4/4P encoded-UVC source (H.264/H.265 up to "
	       "4K60) with multi-camera support.";
}

extern struct obs_source_info dji_uvc_source;

bool obs_module_load(void)
{
	obs_register_source(&dji_uvc_source);
	blog(LOG_INFO, "[dji-uvc] loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[dji-uvc] unloaded");
}
