#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-linkaudio", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Ableton Link Audio source";
}

extern void RegisterLinkAudioSource();

bool obs_module_load(void)
{
	RegisterLinkAudioSource();
	return true;
}