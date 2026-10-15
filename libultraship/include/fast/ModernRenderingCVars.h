#pragma once

// Console variables for the modern rendering features. Defined here rather than as compile definitions so
// adding a setting doesn't rebuild every translation unit; a port can still override any of them.

#ifndef CVAR_MODERN_LIGHTING
#define CVAR_MODERN_LIGHTING "gSettings.ModernRendering.Lighting"
#endif
#ifndef CVAR_MODERN_LIGHTING_WRAP
#define CVAR_MODERN_LIGHTING_WRAP "gSettings.ModernRendering.LightingWrap"
#endif
#ifndef CVAR_MODERN_LIGHTING_SPECULAR
#define CVAR_MODERN_LIGHTING_SPECULAR "gSettings.ModernRendering.LightingSpecular"
#endif
#ifndef CVAR_MODERN_LIGHTING_SHININESS
#define CVAR_MODERN_LIGHTING_SHININESS "gSettings.ModernRendering.LightingShininess"
#endif
#ifndef CVAR_MODERN_LIGHTING_RIM
#define CVAR_MODERN_LIGHTING_RIM "gSettings.ModernRendering.LightingRim"
#endif
#ifndef CVAR_MODERN_AO
#define CVAR_MODERN_AO "gSettings.ModernRendering.AmbientOcclusion"
#endif
#ifndef CVAR_MODERN_AO_RADIUS
#define CVAR_MODERN_AO_RADIUS "gSettings.ModernRendering.AmbientOcclusionRadius"
#endif
#ifndef CVAR_MODERN_AO_INTENSITY
#define CVAR_MODERN_AO_INTENSITY "gSettings.ModernRendering.AmbientOcclusionIntensity"
#endif
#ifndef CVAR_MODERN_AO_DEBUG
#define CVAR_MODERN_AO_DEBUG "gSettings.ModernRendering.AmbientOcclusionDebug"
#endif
