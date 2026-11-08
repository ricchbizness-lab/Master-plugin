# RicchBeatz Master (notes pour Claude Code)
- App standalone JUCE 8.0.4 / CMake / C++17 : mastering assisté d'un fichier audio. Pas de plugin.
- `Source/Dsp.h` : biquad, K-weighting, LUFS/LRA, true-peak (sinc Kaiser 32 pts x4), FFT, spectre 1/3 octave, FIR phase linéaire.
- `Source/MasterEngine.*` : styles, analyse, chaîne (EQ -> multibande -> saturation -> stéréo -> limiteur true-peak + boucle de loudness). Aucune dépendance UI.
- `Source/MainComponent.*` : interface 1000x660 (mise à l'échelle par `ScaledRoot` dans Main.cpp), threads via ThreadPool, lecture A/B via `Player`.
- `Source/Look.h` : palette Nebula, look and feel, i18n (`tr()` : clés = textes français, table EN ; toujours `String::fromUTF8` pour les accents).
- Règle : toute modification du DSP doit repasser `MasterTest selftest` et une comparaison avec `ffmpeg -af ebur128=peak=true` (LUFS et true-peak).
- Limiteur : le plafond interne est ceiling * 0.97 ; le true-peak final est contrôlé et recalé si besoin.
