# Guide Utilisateur Complet — **CPM2 Vinyl Studio**

Éditeur 3D GPU (DirectX 11 + Dear ImGui + ImGuizmo + Engine v5 **Auto-Converge**) pour créer, converger automatiquement par comparaison d'images, placer en 3D sur carrosserie/vitres et injecter des livrées haute fidélité dans **Car Parking Multiplayer 2** (*offline uniquement*).

---

## 1. Installation portable (fonctionne sur n'importe quel PC / disque)

1. Décompressez le dossier `CPM2_Vinyl_Studio` n'importe où (aucun chemin en dur).
2. Assurez-vous d'avoir **Python 3** avec `UnityPy`, `Pillow` et `numpy` pour l'extraction 3D automatique des 224 véhicules :
   ```powershell
   pip install UnityPy pillow numpy
   ```
3. Double-cliquez sur **`Lancer_CPM2_Vinyl_Studio.bat`** (ou `cpm2_studio.exe`).
   - Le Studio détecte **automatiquement** votre installation Steam de **Car Parking Multiplayer 2** (via le registre Steam et `libraryfolders.vdf`), quel que soit le disque (`C:`, `D:`, `E:`, etc.).
   - Si votre jeu est dans un dossier personnalisé, allez dans **Menu `Projet` → `Choisir dossier du jeu CPM 2 (Car Parking 2.exe)...`** : le choix est sauvegardé localement dans `studio_config.json`.

---

## 2. Synchronisation Garage Temps Réel & Doublons de Véhicules

Dans le panneau de gauche (**`Garage & Connexion Jeu (Temps Réel)`**) :
- **Détection automatique du jeu** : indique en vert **`Jeu EN LIGNE (PID ...)`** dès que `Car Parking 2.exe` est lancé.
- **Gestion des doublons dans le garage** : le Studio lit (en lecture seule) les sauvegardes ES3 de votre garage (`%USERPROFILE%\AppData\LocalLow\Olzhas\Car Parking 2\es3\`) et identifie chaque véhicule par son **`CarInstanceId` unique** (ex. `car187 [AI684_84591]` vs un second `car187 [XY999_12345]`).
- **`Sync Jeu Live`** : sonde le jeu en direct pour marquer le véhicule actuellement monté sur le plateau **`*ACTIF*`**.
- **Injection Atomique & Vérification Live** :
  - Un clic sur **`Injecter en Jeu (Atomique)`** sauvegarde d'abord automatiquement vos fichiers ES3 existants dans `work\backups\`, injecte Carrosserie + Vitres + Plaques en une seule passe **uniquement sur l'instance sélectionnée**, affiche le compte-rendu exact (`SUCCÈS / ECHEC`, nombre de calques, backups créés) et vous permet de cliquer **`Voir Rendu Jeu Live (GPU)`** pour contrôler directement la capture `RenderTexture` du jeu !

---

## 3. Boucle Auto-Converge & Diagnostic Perceptuel Honnête

Au lieu d'itérer manuellement à l'aveugle, le mode **`Auto-Converge (Boucle comparative)`** compare automatiquement l'image vectorisée à l'image originale après chaque passe :
1. **Passe 1** : vectorisation initiale + comparaison perceptuelle image-à-image (**SSIM 8×8 sur le premier plan**, **SSIM Focus/Visage**, **Erreur RGB premier plan seul** sans dilution par le fond transparent, **Silhouette IoU**, **Débordements %**, **Trous %**).
2. **Passes 2..N** : la carte d'erreur résiduelle pixel-par-pixel `E(x,y)` est réinjectée pour **amplifier le poids des zones divergentes** (yeux, bouche, contours manqués), tout en ajustant automatiquement `Pénalité débordement`, `Candidats`, `Mutations` et le nombre de passes de polissage Gauss-Newton (`3 → 5`).
3. **Mise à jour en direct & Élitisme strict** : la voiture 3D et l'historique A/B se mettent à jour après chaque passe, la meilleure fidélité est toujours conservée, et vous pouvez cliquer **`Arrêter & Garder le Meilleur`** à tout moment.
4. **Durée calibrée sur votre CPU** : après votre première passe, le Studio calibre sa vitesse réelle sur votre processeur (finies les fausses estimations).

### Lire le diagnostic de fidélité réelle (`0 – 100 %`)
- **`Fidele` (≥ 88 % ET SSIM ≥ 86 % ET Err.Objet ≤ 19)** : très bonne cohérence visuelle et structurelle, prêt pour injection.
- **`Correct` (76 – 87 %)** : bon rendu global ; une passe Auto-Converge supplémentaire ou un Focus plus serré sur le visage affinera les détails.
- **`Moyen` (60 – 75 %)** : détails ou contours incomplets ; suivez le conseil affiché (ex. augmenter le Budget, la Pénalité de débordement ou activer Auto-Converge 4 passes).
- **`Incoherent` (< 60 %)** : ne correspond pas à l'original ; vérifiez d'abord le détourage dans l'onglet **`Damier`** puis lancez **`Anime Portrait HD`** ou **`Ultra Visage (2x2)`**.

---

## 4. Les 6 Presets 1-clic

| Preset | Usage idéal | Auto-Converge |
|---|---|---|
| **Test Rapide (5s)** | Tester rapidement le détourage et le placement 3D (`110` formes) | Off (1 passe) |
| **Anime Portrait HD** *(Recommandé)* | Meilleure fidélité globale sur capot / portière (`280` formes, Engine v5) | **ON** (4 passes, cible 90 %) |
| **Sticker Bord Blanc** | Stickers die-cut à bordure blanche (`Peler bord blanc ON`, `220` formes) | **ON** (3 passes, cible 90 %) |
| **Cel-Shading (Triangles)** | Aplats nets 6 teintes (Triangles `95` + Courbes, `240` formes) | **ON** (3 passes, cible 88 %) |
| **Lineart Manga Unicode** | Vitres / dessins N&B (Triangles `95` + glyphes Unicode `font 0`, `150` formes) | **ON** (3 passes, cible 86 %) |
| **Ultra Visage (2x2)** | Grand portrait HD découpé en 4 tuiles + Focus visage/yeux (`290` formes) | **ON** (4 passes, cible 92 %) |

---

## 5. Raccourcis & Astuces

- **Caméra 3D** : Clic gauche = Orbite | Clic droit ou molette enfoncée = Déplacer (Pan) | Molette = Zoom.
- **ImGuizmo 3D** : **`W`** = Déplacer | **`E`** = Rotation | **`R`** = Échelle | **`Shift + Clic gauche`** sur la voiture = Coller et aligner automatiquement le décal sur la normale 3D du panneau cliqué.
- **Zone Focus Visage/Yeux** : cochez `Zone Focus` puis **cliquez-glissez directement sur l'image d'aperçu** à droite.
- **Jauges de sécurité CPM 2** (en haut) : `Carrosserie ≤ 300`, `Vitres ≤ 150`, `Plaques ≤ 100`.
