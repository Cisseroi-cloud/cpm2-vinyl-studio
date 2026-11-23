# CPM2 Vinyl Studio

Éditeur 3D GPU (**DirectX 11 + Dear ImGui + ImGuizmo**) et moteur de vectorisation **Engine v5 Auto-Converge** pour concevoir, placer en 3D multi-surfaces et injecter des livrées haute précision dans **Car Parking Multiplayer 2** (*offline uniquement*).

## Ce que fait le projet
- **Extraction et rendu 3D complet** de tous les véhicules CPM 2 (carrosserie, vitres, plaques, habitacle, variantes de bodykit) avec vue 3D libre, vue UV planaire et heatmap de déformation UV.
- **Placement 3D libre (ImGuizmo) & routage automatique Carrosserie ↔ Vitres** par projection BVH + Jacobien local 3D → UV, sans couture entre les calques `CarVinyls` (300 max), `WindowVinyls` (150 max) et `PlateVinyls` (100 max).
- **Découpage Multi-Tuiles (`Multi-Tile Tiling`)** jusqu'à `4×4` avec marge anti-couture (`bleed`) et allocation automatique du budget par densité de détails / zone Focus Visage.
- **Moteur de conversion Engine v5 + Auto-Converge** : boucle comparative image-à-image (SSIM 8×8 + erreur premier plan + IoU silhouette) avec 3 profils (Hybride v5, Triangles `95` + Courbes, Triangles `95` + Lineart Unicode), presets 1-clic et historique A/B.
- **Synchronisation garage temps réel & injection atomique** : gestion des doublons par `CarInstanceId` unique, backup ES3 automatique et capture GPU live post-injection.

## Prérequis pour compiler
- Windows 10/11 x64
- **Visual Studio Build Tools** (compilateur MSVC C++17 x64 + Windows SDK DirectX 11)
- **Git** (pour récupérer les dépendances tierces via `tools/fetch_deps.ps1`)
- **Python 3** avec `UnityPy`, `Pillow` et `numpy` (pour l'extraction des modèles 3D depuis les bundles du jeu) :
  ```powershell
  pip install UnityPy pillow numpy
  ```

## Compilation
```powershell
# 1. Télécharger les dépendances C++ (Dear ImGui, ImGuizmo, nlohmann/json, stb)
powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1

# 2. Compiler les 3 binaires (vectoriseur CLI, injecteur DLL, Studio DX11)
powershell -ExecutionPolicy Bypass -File tools\geometrize_cli\build.ps1
powershell -ExecutionPolicy Bypass -File tools\vinyl_injector\build.ps1
powershell -ExecutionPolicy Bypass -File studio\build.ps1

# 3. (Optionnel) Générer l'archive portable prête à partager dans dist/
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -CreateZip
```

## Utilisation rapide (Release clé en main — rien à installer)
Téléchargez **`CPM2_Vinyl_Studio_v1.0.0_Portable.zip`** dans l'onglet **Releases** de ce dépôt, décompressez-le puis double-cliquez sur **`Lancer_CPM2_Vinyl_Studio.bat`**. Aucune installation (ni Python, ni Visual Studio) n'est requise : tout est déjà compilé et autonome.

## Utilisation (depuis les sources)
1. Lancez `studio\build\cpm2_studio.exe` (ou `Lancer_CPM2_Vinyl_Studio.bat` depuis la Release).
2. Le dossier d'installation Steam de CPM 2 est détecté automatiquement (sinon : **Menu `Projet` → `Choisir dossier du jeu CPM 2...`**).
3. Choisissez l'instance exacte du véhicule dans le panneau **Garage**, importez une image (`+ Image`), appliquez un preset 1-clic et lancez **Auto-Converge HQ**.
4. Positionnez le décal en 3D (`W` Déplacer, `E` Rotation, `R` Échelle, `Shift + Clic` pour coller sur un panneau), vérifiez les jauges de quotas en haut puis cliquez **Injecter en Jeu (Atomique)**.

Pour le guide détaillé de tous les curseurs et presets, voir [studio/README.md](studio/README.md).
