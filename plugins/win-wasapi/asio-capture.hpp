#pragma once

#include <obs-module.h>

bool IsASIOCaptureBackendActive();
bool IsASIOCaptureSelection(const char *value);
void GetASIOCaptureDefaults(obs_data_t *settings);
void AddASIOInputCaptureOptions(obs_property_t *prop);
void AddASIOOutputCaptureOptions(obs_property_t *prop);

void *CreateASIOInputCaptureSource(obs_data_t *settings, obs_source_t *source);
void *CreateASIOOutputCaptureSource(obs_data_t *settings, obs_source_t *source);
void DestroyASIOCaptureSource(void *data);
void UpdateASIOCaptureSource(void *data, obs_data_t *settings);
void ActivateASIOCaptureSource(void *data);
void DeactivateASIOCaptureSource(void *data);

obs_properties_t *GetASIOInputCaptureProperties(void *data);
obs_properties_t *GetASIOOutputCaptureProperties(void *data);

void RegisterASIOInput();
void RegisterASIOOutput();