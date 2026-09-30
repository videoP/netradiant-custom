/*
   Simulated map lights.

   A camera preview of what q3map2 -light would do with the map's light
   entities, without compiling anything. Textured surfaces are shaded, per
   pixel, by the same point-light model q3map2 evaluates per luxel:

     inverse square   add = photons / max(16, sqrt(d^2 + extraDist^2))^2 * angle
     linear           add = max(0, angle * photons / 8000 - dist * fade)

   with worldspawn _ambient / _minlight applied around the sum. The constants
   and the flag handling mirror tools/quake3/q3map2/light.cpp, so a value that
   is wrong here is a value that q3map2 reads differently.

   It is a preview, not the compile. What it does not do: cast shadows (a light
   shines through walls), area / surface lights, suns, radiosity bounce, or
   honour non-default -pointscale / -linearscale.
 */

#pragma once

#include "math/vector.h"
#include "math/matrix.h"

#include <cstddef>
#include <vector>

/// \brief The toggle. Persisted; read by the camera draw to request RENDER_SIMLIGHTS.
extern bool g_simLights_enabled;

/// \brief One light, packed as the four vec4 the shader reads.
struct SimLight
{
	Vector3 origin;
	float envelope;       ///< beyond this the light contributes nothing (q3map2's envelope)
	Vector3 colour;       ///< 0..1, normalised unless spawnflag 32
	float photons;        ///< intensity * pointScale (spotScale for spots), in lightmap-byte units
	float fade;           ///< linear lights only
	float angleScale;     ///< _anglescale, 0 = none
	float extraDist;      ///< _extradist
	float radiusByDist;   ///< spots only: cone radius per unit of distance along the axis
	Vector3 direction;    ///< spots only: unit axis, light -> target
	int flags;            ///< kFlagLinear | kFlagAngle | kFlagSpot

	static constexpr int kFlagLinear = 1;
	static constexpr int kFlagAngle = 2;
	static constexpr int kFlagSpot = 4;
};

struct SimLightsFrame
{
	std::vector<SimLight> lights;  ///< in view, nearest first, at most the requested count
	Vector3 ambient;               ///< worldspawn _ambient * _color, lightmap-byte units (0..255)
	Vector3 minlight;              ///< worldspawn _minlight * _color, lightmap-byte units
	std::size_t total = 0;         ///< light entities that light surfaces, whole map
	std::size_t inView = 0;        ///< of those, whose envelope reaches the view volume
};

/*! \brief Read the map's lights and select the ones that can reach the view.

    Walks the top level of the scene graph only (light entities are never
    inside a container), so the cost tracks the entity count, not the brush
    count. Runs once per frame while the toggle is on: edits to a light's keys
    do not all raise a scene change, so caching would show stale values.

    \param maxLights what to shade with. When more are in view the nearest are
                     kept, silently: frame.inView says how many there were.
 */
void SimLights_collect( SimLightsFrame& frame, const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer, std::size_t maxLights );
