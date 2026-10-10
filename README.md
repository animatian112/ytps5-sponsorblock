# youtube-tv — native YouTube-App für PS5-Homebrew

Eine native YouTube-Anwendung für jailbroken PlayStation-5-Konsolen
(Titel-ID **PPSA99350**), gebaut auf dem Build- und Paketierungssystem von
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
mit dem OpenGL-Interface-Kit von
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui).

Die App wird als ZIP gebaut, per **ps5upload** hochgeladen und landet im
Homebrew-Verzeichnis der Konsole (`/data/homebrew/PPSA99350`); der
Homebrew-Launcher (etaHEN/ShadowMount+-Umgebung) startet sie wie jede andere
Homebrew-Titel.

## Funktionen

- **Nativer Controller-Support** mit haptischem Feedback: Rumble beim
  Bestätigen (`0.35`, 60 ms), leiser Impuls beim Fokuswechsel, Tick beim
  Ziehen des Scrubbers. Abschaltbar in den Einstellungen („Haptik“); die
  Lichtleiste folgt der Akzentfarbe (YouTube-Rot).
- **Werbefrei**: Wiedergabe nutzt ausschließlich die signierten
  `googlevideo.com`-Streams aus `streamingData.adaptiveFormats`. Die
  InnerTube-Antwort wird nur auf `playabilityStatus`, `videoDetails` und
  `streamingData` gelesen; `adPlacements`, `adSlots`, `playerAds` und
  `adBreakHeartbeatParams` wertet die App nie aus — es gibt keine Vor- oder
  Zwischenwerbung im Player.
- **Gewohnte YouTube-Oberfläche**: Seitenleiste links mit Icons —
  **Links** am linken Rand des Inhalts übergibt den Fokus an die Leiste,
  **Hoch/Runter** wandert der Cursor (Home → Subscriptions → Settings),
  **X** wechselt den Bereich, **Rechts/Kreis** gibt den Fokus ans Bild
  zurück, **Touchpad** fokussiert die Leiste direkt, **Quadrat** pinnt sie
  offen. Dazu Startseite mit Video-Grid, Suche mit On-Screen-Keyboard und
  **Kanal-Zeile** (Kanal antippen öffnet seine Uploads, Kreis führt zurück),
  Abos, Player mit Transportleiste, Qualitäts-/Codec-Label und „Up next“-Reihe,
  Einstellungen. Die Oberfläche ist einheitlich englisch (kein
  Mehrsprachen-Build) und nutzt **Google Sans Flex** (400/500/700, OFL) plus
  DejaVu Sans Mono für Nummern und den Anmelde-Code. L1/R1 wechselt die
  Bereiche weiterhin direkt.
- **Anmeldung per QR-Code oder TV-Code**: OAuth-2.0-Device-Flow des
  YouTube-TV-Clients. Der Login-Bildschirm zeigt links den QR-Code, rechts den
  Code zum Eintippen auf `google.com/device`. Das Refresh-Token liegt in
  `/download0/hui/auth.json`.
- **Codecs einzeln abschaltbar**: AVC (H.264), VP9 und AV1 in den
  Einstellungen; mindestens einer bleibt aktiv. Streamwahl: höchste Auflösung
  bis zum eingestellten Limit (720p/1080p/1440p/2160p, Standard 1080p),
  Vorzug AV1 → VP9 → AVC. Audio: Opus vor AAC.
- **Software-Decoder**: PacBrew-ffmpeg (7.0.1) demuxt und dekodiert beide
  Spuren; Audio läuft über den Kit-Mixer, Video als OpenGL-Textur.

## Installation auf der Konsole

Vorausgesetzt wird eine jailbroken PS5 mit laufendem ELF-Loader (Port 9021)
und einer Homebrew-Umgebung (elfldr/kstuff/etaHEN, ShadowMount+), wie sie
ps5upload beschreibt.

1. `PPSA99350.zip` aus dem `dist/`-Ordner (oder aus einem GitHub-Release)
   in ps5upload öffnen.
2. Als Ziel **`/data/homebrew/`** wählen; fragt das Tool nach einem Ordnernamen,
   **`PPSA99350`** eintragen. ps5upload entpackt das Archiv in diesen Ordner,
   sodass die App unter `/data/homebrew/PPSA99350/` liegt. Das ZIP ist **flach**:
   `eboot.bin`, `sce_sys/`, `sce_module/` und `assets/` liegen direkt auf der
   obersten Ebene des Archivs (kein umschließender `PPSA99350/`-Ordner), damit
   der von ps5upload angelegte Ordner selbst der App-Ordner wird.
3. Die App im Homebrew-Launcher starten. Scannt der Launcher stattdessen
   `/user/homebrew`, das ZIP dorthin hochladen — das Layout ist identisch.
4. Nach einem Update über ps5upload (oder `make deploy`) zeigt ShadowMount+
   Titel und Icon erst nach einem Neu-Mount bzw. Neustart der Konsole
   aktualisiert — es staged `param.json` und die Startbilder einmalig.

## Releases und Auto-Update

Bezugsquelle ist ausschließlich dieses Repository unter
`https://github.com/haasele/ytps5`: das Git-Remote (`origin`) zeigt dorthin, es
gibt keinen zweiten Upstream. Der Fork setzt auf dem Kit
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) und
dessen Boilerplate
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
auf, beide GPL-3.0-or-later — dieselbe Lizenz wie dieses Projekt.

**Ein Release schneiden:** Die Version steht in `sce_sys/param.json` im Feld
`contentVersion` (`NN.NNN.NNN`, z. B. `01.000.001`) und ist zugleich der
Git-Tag. Also `contentVersion` setzen, committen, `git tag 01.000.001` und
`git push origin 01.000.001`. CI (`.github/workflows/tooling.yml`) baut auf
ubuntu-24.04 mit dem gepinnten clang-18/ninja-Toolchain, prüft das flache
`dist/PPSA99350.zip` und legt bzw. aktualisiert das GitHub-Release für den Tag
mit genau dieser ZIP und `SHA256SUMS`. Tag = Version, Asset = `PPSA99350.zip` —
mehr braucht ein Update nicht.

**In der App:** Beim Start fragt die App die `releases/latest`-Schnittstelle
des Repos ab und vergleicht den Tag mit ihrer eigenen `contentVersion`. Ist das
Release neuer, lädt sie `PPSA99350.zip` über den **ELF-Loader auf
`127.0.0.1:9021`** und übergibt das Archiv an den mitgelieferten
`self-updater.elf`; der prüft Größe und SHA-256, entpackt daneben und tauscht
die Dateien aus, sobald die App sich schließt (der Titel startet dann in der
neuen Version). Voraussetzung ist ein laufender ELF-Loader (elfldr/etaHEN, Port
9021) — ohne ihn findet die Versionsprüfung statt, aber Herunterladen und
Staging melden einen Fehler.

## Selbst bauen

Auf Linux/WSL mit `clang-18`, `ninja`, `make`, `python3`, `curl`, `git`;
im Repo genügt:

```bash
make deps              # Toolchain-Teile, ps5-payload-sdk, ps5-opengl, PacBrew-Sysroot
make                   # dist/PPSA99350/ und dist/PPSA99350.zip
make deploy PS5_HOST=<ip>   # optional: per FTP (Port 2121) direkt auf die Konsole
DEPLOY_DRY_RUN=1 make deploy PS5_HOST=192.0.2.1   # zeigt nur das Ziel
```

`make` benutzt PacBrew-Module für libcurl, OpenSSL und ffmpeg
(`PACBREW_PACKAGES` in `.env` bzw. auf der Kommandozeile). `APP_*`-Schalter
des Boilerplates gelten unverändert; die Identität der App steckt
ausschließlich in `sce_sys/param.json` (`titleId`, `conceptId`, `contentId`,
`contentVersion`).

Wer hier auf einem Rechner ohne System-clang/EGL/curl-Dev-Pakete arbeitet:
die Host-Builds (Tests, Snapshots, Smoke) holen Header und Bibliotheken aus
`.hostdeps/` (curl, GL, EGL, lokales EGL-Vendor-JSON); die
Kreuzkompilierung nutzt das in `.env` gesetzte `PATH`
(`.toolchain/bin` mit clang-18, lld, ninja, clang-format).

## Verifikation ohne Konsole

```bash
make test-unit          # 211 Host-Tests (Kit + yt-Schicht, Sanitizer an)
make test-integration   # Werkzeug-Tests (Build, Deploy, ZIP-Modus)
make host-snapshots     # rendert jede App-Seite als PNG nach build/snapshots
make smoke              # LIVE: Device-Code + echte Suche gegen YouTube (braucht Internet)
make smoke SMOKE_ARGS=login   # LIVE: Anmelde-Fluss inkl. Abbrechen, ohne Handy
```

`make smoke` ist der ehrlichste Test: er holt sich einen echten Device-Code,
stellt eine echte Suchanfrage und listet die gefundenen Videos samt
Thumbnail-URLs. Die Wiedergabe prüft er nicht, weil YouTube für manche Netze
(Rechenzentrums-IPs) auch ohne Anmeldung „Sign in to confirm you're not a
bot“ antwortet — die Konsole meldet sich vorher an, damit spielt es.

`make host-snapshots` rendert die sechs Seiten der App mit Beispieldaten
(kein Netz, kein Decoder) und schreibt sie nach `build/snapshots/`; auf der
Konsole macht die eingebaute Tour dasselbe (siehe
`docs/CONSOLE_VALIDATION.md`).

## Bedienung

| Taste | Wirkung |
| --- | --- |
| Steuerkreuz / linker Stick | Navigieren |
| X | Bestätigen (Kreis bei „Bestätigen tauschen“ in den Einstellungen) |
| Kreis | Zurück, Player verlassen |
| L1 / R1 | Bereich wechseln (Start, Suche, Abos, Einstellungen); im Player ±10 s |
| L2 / R2 | im Grid seitenweise springen |
| Links | Seitenleiste fokussieren (am linken Rand des Inhalts) |
| Hoch/Runter (Leiste) | Bereich auswählen (Home → Subscriptions → Settings) |
| X (Leiste) | Bereich öffnen |
| Touchpad | Seitenleiste fokussieren/einklappen |
| Quadrat | Seitenleiste offen pinnen · Login: neuer Code |
| Options | App beenden |

## Konto, Gast-Modus und Hardware-Decode

**Gast-Zugang ist möglich und der Standard.** Die App startet ohne Anmeldung:
Startseite, Suche und Einstellungen funktionieren, „Abos“ fragt nach dem
Konto, der Login-Bildschirm lässt sich mit Kreis überspringen. Nur die
*Wiedergabe* verlangt auf manchen Netzen eine Anmeldung: YouTube antwortet
nicht angemeldeten Clients dann mit „Sign in to confirm you're not a bot“ —
kein Client (iOS, Android, TV, Web) kommt daran vorbei, das ist eine
Netzwerk-/IP-Entscheidung, keine App-Grenze. Der Player zeigt in dem Fall
„Anmeldung erforderlich“ und **X** führt direkt zur Anmeldung; danach startet
das Video von selbst.

**Anmeldungen bleiben erhalten.** Das Refresh-Token liegt in
`/download0/hui/auth.json` — das ist der Save-Bereich des Titels auf der
Konsole, er überlebt Reboots und jedes Re-Jailbreak (nur ein Löschen der
Titeldaten oder der Konsole entfernt ihn). Zusätzlich:

- Beim Start liest die App `yt-token.json` aus dem App-Ordner
  (`/app0/yt-token.json`, also z. B. `/data/homebrew/PPSA99350/yt-token.json`),
  falls im Sandbox-Speicher nichts liegt — nach einem Wipe einfach die Datei
  wieder hineinlegen (ps5upload/FTP) und die App ist angemeldet.
- Beim Anmelden schreibt sie diese Kopie zurück, wenn der Ordner beschreibbar
  ist, und meldet per Toast, wo sie liegt.
- „Konto sichern“ in den Einstellungen schreibt die Kopie jederzeit erneut.
- Jeder Start erneuert das Token in Ruhe (Refresh), damit Google es nicht als
  unbenutzt verfallen lässt.

**Hardware-Decode:** Die PS5 hat einen fest verdrahteten Videodekoder
(Videodec2), der AVC/HEVC/VP9 nachweislich dekodiert — VP9 auch in 4K60. Diese
App bindet die 26 bekannten Einstiegspunkte beim Bauen als Importe ein
(`tooling/stubs/`, 26/26 NIDs im Paket verifiziert) und protokolliert beim
Start, wie viele davon die Konsole bindet; **aufgerufen wird noch nichts**,
weil die Argument-Layouts nicht öffentlich sind. AV1 hat dort keinen Nachweis
und bleibt Software. Der komplette Stand, der Konsolen-Test und der Weg zur
VP9-Integration stehen in [docs/HARDWARE_DECODE.md](docs/HARDWARE_DECODE.md).

## Client-Wahl und Fehlermeldungen

YouTube akzeptiert angemeldete Anfragen praktisch nur beim TV-Client. Die App
schaltet deshalb automatisch um: ohne Anmeldung fragt sie als Web-Client
(Sign-out-Inhalte), mit Anmeldung als TV-Client und fällt bei einer Absage
(HTTP 400/401/403/410 oder leere Seite) genau einmal auf den Web-Client
zurück. Fehler nennen den Client, der sie verursacht hat
(`http 400 (TVHTML5)`), und jede Anfrage steht mit Client, Status und
Trefferzahl in `yt.log`.

Für die Wiedergabe läuft eine Kette aus sechs Clients (TVHTML5, iOS,
Android-VR, TV-Embedded, Web-Embedded, Web) mit den jeweiligen
Client-Versionen, User-Agents und Zusatzfeldern; `make smoke SMOKE_ARGS=clients`
zeigt das Ergebnis jedes Versuchs.

## Kanal-Seite

Kanal-Karten in den Suchergebnissen öffnen die Kanal-Seite: Avatar, Name,
Abo-Zeile und die **Tabs, die der Kanal selbst meldet** (Home, Videos, Live,
Playlists, Posts …). **Links/Rechts** wechseln den Tab (L1/R1 bleiben die
Hauptbereiche), X öffnet ein Video, Kreis führt zurück zur Trefferliste. Fehlt ein Live-Tab, streamt der
Kanal gerade nicht — die Tab-Liste kommt aus der Antwort, nicht aus einer
festen Reihenfolge. `make smoke SMOKE_ARGS=channel` prüft Header, Tabs und
Tab-Wechsel live.

## Anmeldung

Der Anmelde-Fluss läuft als Geräte-Flow (QR oder TV-Code). Zwei Dinge, die in
früheren Builds kaputt waren und jetzt gepinnt sind:

- Der Flow bleibt in Wartestellung: `make smoke SMOKE_ARGS=login` holt einen
  echten Geräte-Code, prüft, dass der Zustand in Wartestellung bleibt, und dass
  Abbrechen innerhalb von 200 ms beantwortet wird. Der Test läuft ohne Handy.
- Nach erfolgreicher Anmeldung kehrt die App **dorthin zurück, wo der Login
  angestoßen wurde** (Player → Video startet von selbst; Abos → Feed lädt) und
  legt eine Kopie des Tokens ab (siehe „Anmeldungen bleiben erhalten“).

## Wenn die Wiedergabe hängt

Die App schreibt jede Stufe einer Video-Anfrage in eine eigene Logdatei:

- Pfad auf der Konsole: `/download0/hui/dev/yt.log`
  (per FTP lesbar während die App läuft:
  `/mnt/sandbox/PPSA99350_000/download0/hui/dev/yt.log`).
- Zeilen wie `video: request id=…`, `video: player info ok …`,
  `video: selected itag=…`, `video: open begin`, `video: open failed - …`
  zeigen genau, an welcher Stelle es klemmt.
- Der Player nennt die laufende Stufe auch im Bild
  („Loading video – Streams/Decoder“), und eine Anfrage, die 30 s lang weder
  ankommt noch scheitert, wird vom Watchdog beendet
  („Playback timed out“) statt ewig zu laden.

## Grenzen und Hinweise

- **Auflösung**: Software-Decoder; Standard-Limit 1080p. 1440p/2160p sind
  wählbar, können aber ruckeln — das ist erwartetes Verhalten, keine
  Fehlfunktion (Hardware-Decode ist vorbereitet, siehe oben).
- **Anmeldung**: siehe oben — Gast-Modus ohne Konto, Anmeldung nur für die
  Wiedergabe auf „geflagten“ Netzen, Token überlebt Re-Jailbreaks. Erscheint
  kein Device-Code, ist die Client-ID des TV-Clients veraltet; die App liest
  sie beim Anmelden aus dem TV-Bundle, der eingecheckte Wert ist nur ein
  Rückfall.
- **Schrift**: Titel außerhalb Latein/Griechisch/Kyrillisch (z. B. CJK oder
  Emoji) werden als leere Kästen gezeichnet; die gebackenen Fonts decken das
  europäische Set ab.
- **Empfehlungen**: Für angemeldete Konten liefert YouTube den Home-Feed; ohne
  Anmeldung bleibt er auf vielen Netzen leer (die App sagt das an).
- **Rechtliches**: Dies ist ein unabhängiges Homebrew-Projekt ohne Verbindung
  zu Google/YouTube; „YouTube“, „PlayStation“ und „PS5“ gehören ihren
  jeweiligen Inhabern. Lizenz: GPL-3.0-or-later wie das Kit
  (siehe `LICENSE`, `THIRD_PARTY_NOTICES.md`).

## Aufbau

```
src/yt/            Netz- und Logikschicht, plattformunabhängig und host-getestet:
                   http, innertube (TVHTML5/WEB, Adblock), auth (Device Flow),
                   qr, codec (Streamwahl), image, service (Worker-Thread), player
src/app/           Bildschirme: app (Tabs/Tour/Chrome), home, search,
                   subscriptions, settings, login, player, feed_view, thumbs
src/platform/ps5/  Konsolenschicht: Display, Pad, AudioOut, Player (ffmpeg),
                   libcurl-Transport, libc-Shims
host/              PC-Builds: Snapshot-Runner, Tests, Live-Smoke, HTTP-Transport,
                   Player-Stub
```

Die Kit-Dokumentation unter `docs/` (Kit-Referenz, Craft, Komponenten,
Themes, Performance, Konsolen-Validierung) gilt unverändert weiter.
