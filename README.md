# RicchBeatz Master

Application de mastering assisté (Windows / macOS), dans l'esprit de Waves Online Mastering : tu charges un morceau, tu choisis un style, l'app analyse puis masterise, tu compares en A/B à volume égalisé et tu exportes un WAV.

## Utilisation
1. Glisse un morceau (WAV, AIFF, FLAC, MP3) dans « Morceau » ou clique pour parcourir.
2. Optionnel : glisse un morceau de référence. Sa courbe tonale et son volume remplacent ceux du style.
3. Choisis un style (Équilibré, Club, Chaud, Transparent, Rap / Trap), ajuste Volume (LUFS cible), Glue, Punch, Brillance, Largeur.
4. Écoute en direct : chaque réglage (style, potards, référence) s’entend en ~0,5 s pendant la lecture ; le calcul exact finit quelques secondes après. Compare avec ORIGINAL / MASTER (« Volume égalisé » : même volume perçu). EXPORTER… écrit toujours le rendu exact.
5. EXPORTER : WAV 24 bits (par défaut), 16 bits (dither TPDF) ou 32 float.

## Chaîne de traitement (offline, fichier entier)
1. Analyse : volume intégré (LUFS, BS.1770-4), LRA, true-peak (x4), corrélation, spectre en tiers d'octave.
2. EQ à phase linéaire : corrige doucement l'écart vers la courbe cible (±3 dB max, atténuée), plus teinte du style et Brillance. Passe-haut 22 Hz.
3. Compression multibande 3 voies (140 Hz / 3 kHz, Linkwitz-Riley 4), seuils calés sur le niveau du morceau (Glue / Punch).
4. Saturation douce suréchantillonnée x4 (selon le style).
5. Stéréo : basses mono sous 120 Hz, Largeur sur le Side.
6. Limiteur true-peak à anticipation (2 ms), plafond -1 dBTP, boucle jusqu'au volume cible (±0,1 LU).

## Limites connues
- Ce n'est pas l'IA de Waves : c'est une chaîne DSP classique avec analyse adaptative. Les courbes cibles des styles sont génériques ; la référence est plus précise.
- Le mastering ne remplace pas un bon mix : un mix déséquilibré le restera.
- Pas de lecture en temps réel du master pendant le calcul : un morceau de 4 min demande quelques secondes.

## Outils de développement (Linux)
```
cmake -S . -B build-dev -DMASTER_DEV_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-dev -j
build-dev/MasterTest_artefacts/Release/MasterTest selftest                  # tests des briques DSP
build-dev/MasterTest_artefacts/Release/MasterTest in.wav out.wav [style] [lufs] [ref.wav]
xvfb-run -a build-dev/UiTest_artefacts/Release/UiTest in.wav out.wav        # test bout en bout de l'interface
xvfb-run -a build-dev/MasterSnapshot_artefacts/Release/MasterSnapshot out.png in.wav [style] [en] [nomaster] [ref.wav]
```
