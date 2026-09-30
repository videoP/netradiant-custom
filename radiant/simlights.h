/*
   Simulated map lights.

   A camera preview of what q3map2 -light would do with the map's lights,
   without compiling anything. Textured surfaces are shaded, per pixel, by the
   same models q3map2 evaluates per luxel:

     inverse square   add = photons / max(16, sqrt(d^2 + extraDist^2))^2 * angle
     linear           add = max(0, angle * photons / 8000 - dist * fade)
     surface light    add = formFactor(polygon, sample) * value * 3 * areaScale
     sun              add = intensity * angle

   with worldspawn _ambient / _minlight applied around the sum. The constants
   and the flag handling mirror tools/quake3/q3map2/light.cpp, so a value that
   is wrong here is a value that q3map2 reads differently.

   Light sources:
     - light entities (point, spot, linear)
     - q3map_surfacelight shaders on brush faces and patches, as triangle area
       lights with q3map2's exact point-to-polygon form factor, plus its
       backsplash
     - q3map_sun, when shadows are on (an unshadowed sun lights the inside of
       every room)

   Shadows (simshadows.h) are shadow maps: a cube map for each of the nearest
   few point / spot / area lights and one orthographic map for the sun. They
   approximate q3map2's traced shadows; they do not match them texel for texel.

   It is a preview, not the compile. What it does not do: radiosity bounce,
   model surface lights, skylights, light styles, twosided emitters, or honour
   non-default -pointscale / -linearscale / -areascale.
 */

#pragma once

#include "math/vector.h"
#include "math/matrix.h"

#include <cstddef>
#include <vector>

/// \brief The toggles. Persisted; read by the camera draw.
extern bool g_simLights_enabled;
/// \brief Shadow maps for the nearest lights and the sun. Only meaningful with g_simLights_enabled.
extern bool g_simShadows_enabled;

/// \brief One light, packed as the four vec4 the shader reads.
struct SimLight
{
	Vector3 origin;       ///< point/spot: position. area: centre of the polygon's bounds, nudged off its plane
	float envelope;       ///< beyond this the light contributes nothing (q3map2's envelope)
	Vector3 colour;       ///< 0..1, normalised unless spawnflag 32
	float photons;        ///< point/spot: intensity * scale, lightmap-byte units. area: q3map2's light->add
	float fade;           ///< linear lights only
	float angleScale;     ///< _anglescale, 0 = none
	float extraDist;      ///< _extradist
	float radiusByDist;   ///< spots only: cone radius per unit of distance along the axis
	Vector3 direction;    ///< spots only: unit axis, light -> target
	Vector3 verts[3];     ///< area only: the emitting triangle, wound as q3map2 winds a face (clockwise seen from its front)
	int flags;            ///< kFlag* below
	int shadowSlot = -1;  ///< which shadow cube this light is checked against, or -1; set by SimShadows_frame

	static constexpr int kFlagLinear = 1;
	static constexpr int kFlagAngle = 2;
	static constexpr int kFlagSpot = 4;
	static constexpr int kFlagArea = 8;
	static constexpr int kFlagTwoSided = 16;
	static constexpr int kKindMask = 31;
	static constexpr int kSlotScale = 32; ///< flags = kinds + ( shadowSlot + 1 ) * kSlotScale, as the shader unpacks it
};

/// \brief q3map_sun, as q3map2's CreateSunLight makes it.
struct SimSun
{
	bool present = false;
	Vector3 direction{ 0, 0, 1 }; ///< unit, from a surface towards the sun
	Vector3 colour{ 1, 1, 1 };    ///< normalised
	float photons = 0;            ///< the shader's intensity
};

/// \brief Most shadow cube maps in a frame. Also what the shader is built for.
constexpr std::size_t c_simShadowCubesMax = 6;

/// \brief A shadow cube that is ready to be sampled.
struct SimShadowCube
{
	unsigned texture = 0;
	Vector3 origin;
	float farPlane = 0;   ///< what the cube was rendered out to; the near plane is c_simShadowNear
};

/// \brief Distance of the cube faces' near plane. The shader reconstructs distance with the same number.
constexpr float c_simShadowNear = 2.f;

struct SimLightsFrame
{
	std::vector<SimLight> lights;  ///< in view, nearest first, at most the requested count
	Vector3 ambient;               ///< worldspawn _ambient * _color, lightmap-byte units (0..255)
	Vector3 minlight;              ///< worldspawn _minlight * _color, lightmap-byte units
	std::size_t total = 0;         ///< lights that light surfaces, whole map
	std::size_t inView = 0;        ///< of those, whose envelope reaches the view volume

	SimSun sun;                    ///< only present when shadows are on

	/* Filled in by SimShadows_frame. */
	SimShadowCube cubes[c_simShadowCubesMax];
	std::size_t cubeCount = 0;
	int cubeSize = 0;              ///< texels per cube face edge
	bool sunShadowed = false;
	unsigned sunTexture = 0;
	Matrix4 sunMatrix;             ///< world -> the sun map's [0,1]^3
	float sunTexel = 0;            ///< world units per texel
	float sunRange = 0;            ///< world units the map's depth covers
};

/// \brief Something in the scene changed. Cached surface lights and shadow maps are stale.
void SimLights_sceneChanged();
/// \brief Bumps with every SimLights_sceneChanged.
unsigned SimLights_generation();

/*! \brief Read the map's lights and select the ones that can reach the view.

    Light entities are read from the top level of the scene graph, so the cost
    tracks the entity count, not the brush count, and every frame: edits to a
    light's keys do not all raise a scene change, so caching would show stale
    values. Surface lights need the brushes, so they are gathered once per scene
    change and cached.

    \param maxLights what to shade with. When more are in view the nearest are
                     kept, silently: frame.inView says how many there were.
 */
void SimLights_collect( SimLightsFrame& frame, const Matrix4& modelview, const Matrix4& projection, const Vector3& viewer, std::size_t maxLights );

/// \brief Texels across the textures the shader reads lights, clusters and lists from.
constexpr int c_simClusterTexW = 4096;
/// \brief Most lights one cluster keeps. The shader loops to this.
constexpr std::size_t c_simClusterMax = 192;
/// \brief Texels a light takes in the light texture; a power of two so a light never straddles a row.
constexpr int c_simLightTexels = 8;

/*! \brief The frame's lights, sorted into a grid of screen tiles by depth slices,
    as the flat arrays the fragment shader reads from textures.

    A pixel looks up its own cluster and loops over only the lights that reach
    it, so how many lights the map has does not matter, only how many overlap.
    That is what makes the lighting the same whichever way the camera turns.
 */
struct SimClusters
{
	int tilesX = 0, tilesY = 0, slices = 0, tilePx = 0;
	float zNear = 0;               ///< slice = log( depth / zNear ) * depthScale
	float depthScale = 0;
	std::size_t lightCount = 0;
	std::size_t indexCount = 0;
	std::size_t overflow = 0;      ///< list entries dropped because a cluster was over c_simClusterMax
	std::vector<float> lightTexels;   ///< c_simLightTexels * 4 floats a light, as SimLights_pack
	std::vector<float> clusterTexels; ///< ( first index, count ) a cluster
	std::vector<float> indexTexels;   ///< light numbers, cluster after cluster

	/* what the above was built from, so an unchanged view is not built again */
	bool built = false;
	Matrix4 builtModelview, builtProjection;
	int builtWidth = 0, builtHeight = 0;
};

/// \brief One light as the texels the shader reads, for \p out to hold c_simLightTexels * 4 floats.
void SimLights_pack( const SimLight& light, float* out );

/// \brief Sorts frame.lights into clusters for a view of \p width by \p height pixels.
void SimLights_buildClusters( SimClusters& out, const SimLightsFrame& frame, const Matrix4& modelview, const Matrix4& projection, int width, int height );
