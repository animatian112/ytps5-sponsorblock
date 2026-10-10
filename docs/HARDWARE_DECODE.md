# Hardware-Video-Decode (Videodec2) — Stand und Weg

Diese Seite hält fest, was auf der PS5 **bewiesen** ist, was in dieser App
**gebaut** ist und was für die vollständige Integration noch fehlt. Sie ist
bewusst konservativ: nichts gilt als möglich, was nicht auf einer Konsole
gelaufen ist.

## Was bewiesen ist

Die PS5 dekodiert Video nicht auf den Shader-Kernen, sondern im
Fixed-Function-Block **Videodec2**. Konsolen-validierte Messungen liegen im
Research-Projekt [ps5-hardware-video-decoding-research](https://github.com/blackbearreloaded/ps5-hardware-video-decoding-research)
vor (H.264 High, HEVC Main/Main10, VP9 Profile 0/2; 1080p–4K; Zero-Copy der
Dekoder-Fläche in AGC; VP9 4K60 stabil bei 59,95 fps). **AV1 ist dort nicht
etabliert** — für AV1 gibt es bislang keinen Nachweis, also bleibt es bei
Software (ffmpeg) mit dem Auflösungs-Cap.

Für diese App ist damit die Zielreihenfolge klar: **VP9 zuerst**, dann
AVC/HEVC; AV1 spielt für den Hardware-Pfad keine Rolle.

## Was in dieser App gebaut ist

- `tooling/stubs/libSceVideodec2.c` — Import-Stub mit allen 26 öffentlich
  bekannten Einstiegspunkten (`sceVideodec2InitLibrary`, `CreateDecoder`,
  `CreateHevcDecoder`, `PresetDecoderConfigInfo{,Avc,Hevc,Vp9}`,
  `QueryDecoderMemoryInfo`, `MapDirectMemory`, `Decode`, `GetVp9PictureInfo`,
  …). Der Stub trägt nur Namen; die NIDs leitet die Toolchain daraus ab.
- `tools/build-import-stubs.sh` + `APP_IMPORT_STUBS` — `make` baut den Stub
  und bindet ihn. Verifiziert: alle **26 NIDs** stehen als Importe im
  ausgelieferten `dist/PPSA99350/eboot.bin`.
- `src/platform/ps5/videodec_probe.cpp` — referenziert jeden Einstiegspunkt
  (volatile Tabelle, damit kein Optimierer die Importe entfernt) und schreibt
  beim Start eine Zeile ins Log:
  `[YTV] videodec2 imports: <n>/26 reachable`.
  Aufrufen tut die App **nichts** davon.

## Was auf der Konsole zu prüfen ist (nächster Schritt)

1. **Erreichbarkeit:** App starten und das Log lesen (FTP:
   `/mnt/sandbox/PPSA99350_000/download0/hui/dev/…`, Konsole per Dev-Menü).
   - `26/26 reachable` → die Bibliothek ist für Homebrew-Titel ladbar, die
     Stub-/NID-Kette stimmt. Weiter mit 2.
   - `0/26` → die Symbole bleiben null. Dann muss die Bibliothek erst geladen
     werden (`sceSysmoduleLoadModule` mit der passenden Modul-ID) oder die
     NIDs der realen Firmware weichen vom Namensschema ab; beides braucht einen
     Firmware-Dump auf deiner Konsole.
2. **Signatur-/Struct-Layout:** Erst wenn 1 grün ist, lohnt der Aufbau einer
   Session. Die Research-Doku beschreibt das Verhalten (16-KiB-aligned
   Direct-Memory-Pool, komprimierte Access Units, callereigene Frame-Slots,
   Depth 1) — die Argument-Layouts müssen aber empirisch bestätigt werden
   (InitLibrary → QueryLibraryInfo → CreateDecoder mit VP9-Preset →
   QueryDecoderMemoryInfo → MapDirectMemory → Decode → GetVp9PictureInfo →
   Flush/Reset → DeleteDecoder → TermLibrary).
3. **Präsentation:** Der bewiesene Weg übergibt den Dekoder-Zeiger an AGC.
   Unser Renderer ist OpenGL (ps5-opengl/Mesa→AGC); ob eine dekodierte Fläche
   als GL-Textur importierbar ist, ist nicht dokumentiert. Zwei Optionen:
   Import über den Mesa-Interop-Pfad (im Kit-Header `mesa_glinterop.h`
   vorhanden, Nutzung unbestätigt) oder — wie im Research-Projekt — eine
   eigene AGC-Präsentation, die den Kit-Renderer während des Videos pausiert.

## Warum Software-Decode der Default ist

- Es gibt **keine öffentliche Bibliothek/Header/NID-Tabelle** für Videodec2;
  der Stub in diesem Repo ist die erste veröffentlichte Anbindung. Ohne
  bestätigte Argument-Layouts wäre jeder Aufruf geraten.
- AV1 fehlt im Hardware-Pfad (siehe oben), Software ist dort ohnehin Pflicht.
- Der Software-Weg (ffmpeg → GL-Textur) funktioniert heute für AVC/VP9/AV1 bei
  1080p und ist die Rückfallebene, sobald Hardware VP9/AVC übernimmt.

## Erwarteter Gewinn (aus den Research-Messungen)

| Codec | Auflösung | Software (heute) | Hardware (belegt) |
| --- | --- | --- | --- |
| H.264 | 1080p60 | trägt | 2,27 ms Dekode, 60,00 fps |
| VP9 | 1080p60 | trägt | 5,53 ms Dekode, 173 fps Batch |
| VP9 | 4K60 | grenzwertig | 59,95 fps stabil, 16,7 ms AU→Flip |
| HEVC | 4K60 | grenzwertig | 60,36 fps, 5,46 ms Dekode |
| AV1 | jede | Software | kein Nachweis |
