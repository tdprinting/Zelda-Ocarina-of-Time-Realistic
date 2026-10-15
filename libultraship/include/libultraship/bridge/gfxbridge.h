#pragma once

#include "stdint.h"
#include "fast/ucodehandlers.h"
#include "ship/Api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sets the native (un-scaled) rendering resolution used by the graphics backend.
 *
 * @param width  Native framebuffer width in pixels.
 * @param height Native framebuffer height in pixels.
 */
API_EXPORT void GfxSetNativeDimensions(uint32_t width, uint32_t height);

/**
 * @brief Prepares the graphics backend to sample the pixel depth at screen coordinate (@p x, @p y).
 *
 * Call this before GfxGetPixelDepth() to ensure the depth value is ready.
 *
 * @param x Screen X coordinate in pixels.
 * @param y Screen Y coordinate in pixels.
 */
API_EXPORT void GfxGetPixelDepthPrepare(float x, float y);

/**
 * @brief Returns the pixel depth at screen coordinate (@p x, @p y).
 *
 * Must be called after GfxGetPixelDepthPrepare() for the same coordinates.
 *
 * @param x Screen X coordinate in pixels.
 * @param y Screen Y coordinate in pixels.
 * @return Depth value in the range [0, 65535] (16-bit fixed-point).
 */
API_EXPORT uint16_t GfxGetPixelDepth(float x, float y);

/**
 * @brief Re-uploads a material map at the start of the next frame.
 *
 * For mods that rewrite a map's pixels at runtime (for example wetness or mud on a character). @p mapPath is
 * the map's path as written in the material manifest (a texture resource or an image file). Edit the
 * resource's ImageData in place, then call this; maps no material has used yet are skipped (they are read
 * fresh on first use). Safe to call from any thread.
 *
 * @param mapPath Path of the normal, roughness-metal or skin-mask map.
 */
API_EXPORT void GfxRefreshMaterialMap(const char* mapPath);

#ifdef __cplusplus
}
#endif
