#pragma once
#include "car_model.h"
#include "vinyl_types.h"
#include <string>
#include <vector>

namespace cpm {

// Projects all enabled decals & texts in LiveryProject onto CarVinyls, WindowVinyls, and PlateVinyls.
// Automatically detects Body <-> Window overlaps via BVH raycasting and routes shapes to both surfaces
// using exact local 3D-to-UV Jacobians for continuous seamless visual alignment!
void projectLivery(
    const CarModel& car,
    const SpriteCatalog& catalog,
    LiveryProject& project,
    ProjectedLivery& outLivery
);

// Snap a decal's 3D transform cleanly onto a raycast hit point & surface normal
void snapDecalToHit(DecalItem& decal, const RayHit& hit);

// Import / Export native CPM2 layer files (*_layers.txt) compatible with vinyl_injector & build_livery.py
bool loadLayersTxt(const std::string& path, std::vector<VinylLayer>& outLayers);
bool saveLayersTxt(const std::string& path, const std::vector<VinylLayer>& layers, int hardQuota);

// Import shapes JSON produced by geometrize_cli / prepare_image.py into a DecalItem
bool loadShapesJson(const std::string& path, DecalItem& decal);

// Save / Load complete Studio project (.cpmproj.json)
bool saveProjectJson(const std::string& path, const LiveryProject& project);
bool loadProjectJson(const std::string& path, LiveryProject& project);

} // namespace cpm
