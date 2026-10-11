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

Die Argument-Layouts sind keine Rekonstruktion mehr. Die Session in
`src/platform/ps5/videodec.cpp` folgt der öffentlich auf Hardware
verifizierten Sequenz und nennt ihre Quellen selbst:

- **EVO-PLAYER-PS5** (`sainsaji`),
  `projects/evoplayer/media/include/sce/sce_videodec2.h` — Struct-Layouts,
  Codec-/Level-Konstanten und die Speicher-Typisierung.
- **`docs/evo-pro/videodec2-abi.md`** — die exakte Bring-up-Reihenfolge und die
  Protection-Tabelle je Region (hardware-verifiziert auf PS5 12.70 am
  2026-09-01: 1080p-H.264-IDR bis NV12-Frame, jeder Aufruf `rc=0`).
- Beide schreiben ihrerseits **blackbearreloaded/ProsperoLight** fort, den
  Moonlight-Client, der auf dem VCN-Block dekodiert.

Gebaut ist damit:

- **Import-Kette unverändert:** `tooling/stubs/libSceVideodec2.c` bleibt der
  Stub mit allen 26 öffentlich bekannten Einstiegspunkten; `make` bindet ihn
  über `APP_IMPORT_STUBS` ein, alle 26 NIDs stehen im
  `dist/PPSA99350/eboot.bin`.
- **Kernel-gemappte Regionen (Typ 12):** Der CPU-Workspace liegt in **named
  flexible memory** (Protection `0x03`), alle übrigen Regionen (compute, gpu,
  cpu_gpu, frame pool) werden mit
  `sceKernelAllocateDirectMemory(…, type 12, alignment 0x4000)` +
  `sceKernelMapDirectMemory` gemappt — mit eigener Protection je Region
  (`0x33` CPU r/w + GPU, `0x32` GPU).
- **Compute-Queue zuerst:** `QueryComputeMemoryInfo` → Größe mappen →
  `AllocateComputeQueue` mit `resource_type = Compute`; der Handle steckt
  anschließend in der Decoder-Konfiguration.
- **Explizites Profil/Level je Codec:** AVC `profile_idc 100` (High) und VP9
  Profile 0, Level x10 je Auflösung (AVC 51/52, VP9 41/51), Geometrie auf 16
  gerundet (1080p als 1920×1088), `pipeline_depth = 1`, `cpu_affinity 0x3F`,
  `cpu_priority 700`.
- **`Reset` nach `CreateDecoder`:** Die Session meldet sich erst danach als
  bereit.
- **Validierter `Decode`:** Ein Bild zählt nur bei
  `out.valid && !out.error && picture_count > 0` als Erfolg, und die
  zurückgegebene Fläche muss ein exaktes Mitglied des caller-eigenen
  Frame-Pools sein; liefert die erste AU bei Depth 1 keins, folgt `Flush`.
- **Jeder Schritt loggt:** Jede Phase schreibt genau eine Zeile mit Name,
  Rückgabewert und Import-Zählung; der **erste Fehlschlag nennt den Schritt**
  in der Fehlermeldung, und die Session ruft nie einen null-Import auf.
- **Startup-Probe bleibt:** `videodec_probe.cpp` schreibt beim Start
  `[YTV] videodec2 imports: <n>/26 reachable` — die erste Antwort auf „ist
  libSceVideodec2 für einen Homebrew-Titel ladbar?“.
- **`videodec_supports()` = AVC + VP9:** nur für diese beiden ist ein
  konsolen-bewiesener Pfad vorhanden; AV1 bleibt ohne Nachweis bei Software.
  (HEVC hat einen belegten Pfad, `yt::Codec` führt HEVC aber nicht.)

Nicht bewiesen bleibt die **Präsentation**: Die Session beschreibt ein
dekodiertes Bild, bindet es aber nicht selbst. Ob eine Dekoder-Fläche als
GL-Textur importierbar ist (Mesa-Interop, `mesa_glinterop.h`, Nutzung
unbestätigt), ist weiterhin offen — der Player behandelt genau diesen fehlenden
Import als Hardware-Fehlschlag und fällt auf Software zurück.

## Was ein Konsolen-Lauf zeigt

Mit `Decoding: Auto` oder `Hardware` und einem AVC-/VP9-Stream schreibt der
Lauf der Reihe nach in `yt.log`:

1. **Startup:** `[YTV] videodec2 imports: <n>/26 reachable`. `26/26` heißt: die
   Bibliothek ist für Homebrew ladbar und die NID-Kette stimmt; `0/26` heißt:
   kein Symbol gebunden, die Session verweigert dann jeden Schritt *namentlich*,
   statt einen Null-Zeiger zu rufen.
2. **Compute-Queue:** die Zeile `[YTV] videodec2 compute queue: <ptr> (<bytes>
   bytes)` — davor `QueryComputeMemoryInfo`, das Mappen der Compute-Region und
   `AllocateComputeQueue` (`resource_type = Compute`).
3. **Die vier Direct-Memory-Regionen mit `type=12`:** `region compute: …
   type=12 prot=0x33`, `region gpu: … type=12 prot=0x32`, `region cpu_gpu: …
   type=12 prot=0x33`, `region frame: … type=12 prot=0x32` (dazu `region cpu: …
   prot=0x03` — der CPU-Workspace in flexible memory).
4. **`CreateDecoder` / `Reset`:** `[YTV] videodec2 sceVideodec2CreateDecoder:
   0x00000000 …` und `… sceVideodec2Reset: 0x00000000 …`; jeder Schritt loggt
   seinen Rückgabewert, ein negativer nennt den Schritt in der Fehlermeldung.
5. **Entweder ein dekodiertes Bild oder der erste fehlgeschlagene Schritt:**
   `Decode` gilt nur bei `valid && !error && picture_count > 0` als Erfolg. In
   diesem Build stoppt der noch fehlende Bild-Import (Dekoder-Fläche →
   GL-Textur) jedes Hardware-Video, das Log zeigt dann `ffmpeg: hardware decode
   unavailable: hardware picture import unavailable, using software`, und die
   Wiedergabe läuft in Software. Ein Hardware-Bild ist erst dann „grün“, wenn
   diese Zeile ausbleibt.

## Warum Software-Decode der Default ist

Der Default ist die neue Einstellung **`Decoding: Software`** (in
`src/core/settings.cpp` als Byte im Save-Format; `Auto` und `Hardware` sind
opt-in und werden mitgelesen). Gründe:

- Die Argument-Layouts werden auf der Konsole noch validiert: Die Session folgt
  zwar der veröffentlichten, hardware-verifizierten Sequenz, aber ob *diese
  App* die Regionen mit den Rechten und Größen der Firmware bekommt, entscheidet
  erst ein Konsolen-Lauf. Bis dahin bleibt der Default der Pfad, der heute
  überall trägt.
- Der Bild-Import (siehe oben) ist noch nicht belegt, also bringt der
  Hardware-Pfad heute kein Bild auf den Schirm; die automatische Rückfallebene
  ist Software.
- AV1 hat keinen Hardware-Pfad: Software ist dort ohnehin Pflicht, und im Modus
  `Hardware` wird AV1 gar nicht mehr gewählt.
- Der Software-Weg (ffmpeg → GL-Textur) trägt heute AVC/VP9 bei 1080p. Jeder
  fehlgeschlagene Hardware-Schritt wird geloggt, und `open()` scheitert nie am
  Hardware-Pfad — Software übernimmt.

## Erwarteter Gewinn (aus den Research-Messungen)

| Codec | Auflösung | Software (heute) | Hardware (belegt) |
| --- | --- | --- | --- |
| H.264 | 1080p60 | trägt | 2,27 ms Dekode, 60,00 fps |
| VP9 | 1080p60 | trägt | 5,53 ms Dekode, 173 fps Batch |
| VP9 | 4K60 | grenzwertig | 59,95 fps stabil, 16,7 ms AU→Flip |
| HEVC | 4K60 | grenzwertig | 60,36 fps, 5,46 ms Dekode |
| AV1 | jede | Software | kein Nachweis |
