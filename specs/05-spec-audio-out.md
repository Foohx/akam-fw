# Akam — Spécification : sortie audio (parler par le haut-parleur de la caméra)

Statut : **mise à jour après la capture du firmware d'origine (§1.3, qui prime sur tout ce qui contredit en §1.2, §2.1, §3 et §11)**. Rédigée le 2026-11-10 (agent SPEC, branche `docs/spec-audio-out`, partie de `dev`). Cible : V1.1 (brief §« Audio sortant »). Seule cette spec est en français ; code, messages, README et docs sont en anglais. **Rien ici n'est validé sur matériel** : chaque point marqué « à vérifier » est une inconnue à lever par les tests de §10.

> **Mise à jour du 2026-11-10 (spec 07, `specs/07-spec-standard-protocols.md`, décision utilisateur D1 étape 1) : l'entrée réseau devient le rétrocanal audio ONVIF sur le port RTSP 554** (`Require: www.onvif.org/ver20/backchannel`, piste SDP `a=sendonly` PCMU/PCMA 8 kHz). Il **remplace** le port de parole 8092 et l'exigence HMAC pour l'audio ; la parole exige le **mot de passe RTSP Digest** (sans mot de passe, la piste n'est pas proposée).
> - **Remplacé (ne pas implémenter)** : §0 (port 8092), §4 et §4.1 (protocole `TALK`/`AUTH`, verrou 8092), §5 (`./akam talk|say` par 8092 et HMAC ; remplacés par le client hôte `akam-talk` + les commandes `./akam audio`, spec 07 §1.5), §5.3 et §5.4 (étape D devient l'étape 1), §8 (sécurité du port 8092), les tests §9.1 « Protocole `talk` » et B1/B4/B6, les tâches **T3** et **T5** (§10), les décisions D3, D6 et D10 (§11), et la décision « Auth talk (option 2) » en fin de fichier.
> - **Réévalué** : le half-duplex du §4.3 (micro coupé tant qu'un locuteur est connecté) devient un silence du micro seulement pendant la parole réellement jouée (détection d'énergie, maintien 300 ms), réglage `audio.talk_duplex` (spec 07 §1.4).
> - **Reste valable** : §1 à §3 (matériel, séquence stock, étape A), §4.2 (tampon de gigue), §6 (clés `audio.out_*`, écrites par SSH/MQTT/ONVIF et non plus par le port 8091 distant, spec 07 §4.5), §7 (persistance, via `ctl_store`), §9.1 pour G.711/gigue/périphérique factice/volume, T1, T2, T4, T6 (adaptées), D1, D2, D4, D5, D7 à D9, D11.
> - **Implémenté (2026-11-10, branche `feature/rtsp-backchannel`, tests hôte seulement, pas encore de test matériel)** : l'entrée réseau par le rétrocanal ONVIF, le half-duplex par détection d'énergie, `audio.talk_enable`, `audio.talk_duplex`, les compteurs `audio.out_underruns/drops/buf_ms` et `audio.talk_*`, le client hôte `akam-talk`, `./akam talk` et `./akam say` ; le silence G.711 n'allume plus l'ampli (seuil `AO_AUDIBLE_PEAK`). Détail : spec 07 §1.10, utilisation : `docs/two_way_audio.md`. Non fait : `./akam audio ...` par SSH (le port 8091 reste ouvert jusqu'à l'étape 4 de la spec 07).
> - Clés ajoutées par la spec 07 : `audio.talk_enable` (interrupteur logiciel de la parole, distinct de `audio.out_enable` qui ouvre le périphérique au démarrage), `audio.talk_duplex`.

## 0. Objectif et principe

Parler depuis un PC (puis un téléphone / Home Assistant) dans le haut-parleur intégré de la caméra, en push-to-talk, le plus simplement possible.

Principe retenu : **intégrer la lecture dans `ak_rtsp`** (le processus qui possède déjà micro et lecteur), pas d'outil de lecture séparé. Un flux **G.711 µ-law 8 kHz** arrive sur un **port TCP dédié (8092, ouvert par défaut avec la sortie audio ; authentification HMAC obligatoire, voir les décisions utilisateur)**, passe dans un petit tampon de gigue, est décodé et écrit à la place du silence que `ak_rtsp` écrit déjà sur le chemin de lecture. Côté PC, `./akam talk` capture le micro avec `ffmpeg` et envoie le flux.

Hors périmètre : annulation d'écho acoustique, AAC/MP3/Opus, 16 kHz, TLS, rétrocanal RTSP/ONVIF (étape D, §5.4, reportée), mixage de plusieurs locuteurs.

## 1. Constats (sources lues en lecture seule, rien exécuté)

### 1.1 Ce que fait déjà `ak_rtsp` (`src/ak_rtsp/audio.c`, `ak_ioctls.h`, `ak_rtsp.h`)

- `audio_start()` (appelé par `main.c`, non fatal) ouvre le micro `/dev/pcmC0D0c` puis, **après** le démarrage du thread de capture, `audio_init_playback()` ouvre **`/dev/pcmC0D0p` en `O_WRONLY`** et `/dev/pcmC0D0l` (boucle d'écho, jamais configurée).
- `audio_configure_playback()` appelle, **tous en non fatal** : `SET_STATE(0)`, `SET_RATE(8000)`, `SET_PERIODS(4)`, `SET_STATE(4)`. `SET_MODE(0)` est **volontairement sauté** : « crashes the kernel » à chaque essai (commentaire d'origine : cause racine non élucidée). Aucun `GET_PARS`/commit n'est fait sur le fd de lecture.
- `audio_playback_loop()` (thread, vit toute la vie du processus) écrit en boucle un bloc de **512 octets de silence** (`AUDIO_PERIOD_SAMPLES` = 256 échantillons mono 16 bits), borné à 5 échecs consécutifs puis arrêt silencieux de ce thread. Rôle documenté : « garder un flux frère actif » pour la capture ; la sortie audible est explicitement hors périmètre.
- **Il n'y a ni ioctl de haut-parleur, ni GPIO d'ampli, ni gain, ni volume** dans `ak_rtsp` (`grep gpio` : seulement `ircut.c`, GPIO 65/66/70 via sysfs). Le haut-parleur est donc probablement **coupé** aujourd'hui, et le périphérique de lecture est **déjà ouvert en permanence** par `ak_rtsp`. Le pilote refuse une seconde ouverture (chaîne `ERR! plackback device is already open!` dans `ak_pcm.ko`) : un lecteur séparé type `aoplay` **ne peut pas** tourner en même temps (et le watchdog relance `ak_rtsp` si on le tue). D'où l'intégration.
- Inconnu à vérifier : l'écriture de silence actuelle réussit-elle ? (le thread se tait après 5 échecs ; chercher `[audio] playback write` dans le journal série/`LOG` 8091.)
- `aec_custom.c` n'a rien à voir : c'est de l'**auto-exposition** (AE), pas de l'annulation d'écho.

### 1.2 Pilote `ak_pcm.ko` et firmware d'origine (copie locale propriétaire, lecture seule, hors dépôt)

Chaînes de `modules/ak_pcm.ko` et de `bin/anyka_ipc`, croisées avec `aoplay.c` du projet de référence ([berobloom/LSC-1080p-Anyka-Decloud](https://github.com/berobloom/LSC-1080p-Anyka-Decloud), même SoC AK3918AV130L, noyau 4.4.302) :

| nr (`'P'`) | Nom dans `ak_ioctls.h` (Akam) | Nom dans le pilote / `aoplay.c` | Usage lecture |
|---|---|---|---|
| `0xE0` | `GET_PARS` (déjà noté « mal nommé ») | `IOC_PREPARE` (`copy_from_user`, 28 o) | **alloue** tampon matériel + cache ; valide `periods`, `period_bytes`, `rate` |
| `0xE1` | `SET_MODE` | `IOC_RESUME` (« start again ») | démarre le DMA |
| `0xE3` | `RESET_BUF` | `IOC_RSTBUF` | vide |
| `0x10` | `SET_STATE` / `GET_STATE` | `IOC_SET_SOURCES` / `GET_SOURCES` (« playback source:%d -> %d ») | routage du mixeur : **4 = route de lecture audible (capture stock, §1.3)**, 0 = route coupée ; 6 (référence) non vérifié |
| `0x30` | `SET_PERIODS` / `GET_PERIODS` | `IOC_SET_GAIN` / `GET_GAIN` (« playback HP gain ») ; sur le fd capture : gain micro / linein | gain matériel 0..5 (à vérifier) |
| `0x81` | `GET_CAPS` | `IOC_SET_SPEAKER` (**`_IOW`, `0x40045081`**, voir §1.3 : le `0x80045081` supposé ici renvoie `EINVAL`) | **active l'ampli** (« SPEAKER @%d(lv %d) ») |

Conséquences (hypothèses fortes, à confirmer sur matériel ou par Ghidra de `ak_pcm.ko`) :

1. ~~Seule la forme `0x80045081` existe~~ : **faux**, corrigé en §1.3. C'est `0x40045081` (`_IOW` standard) qui est `SET_SPEAKER`.
2. **Le crash de `SET_MODE` sur le fd de lecture est très probablement `IOC_RESUME` sans `IOC_PREPARE` préalable** : `ak_rtsp` n'a jamais committé les paramètres de lecture (le pilote vérifie `play_rt is NULL` à certains endroits, pas forcément ici). `aoplay` fait `PREPARE` puis `SOURCES` puis `RESUME`. La séquence actuelle (`SET_STATE(4)` = source DAC, `SET_PERIODS(4)` = gain 4) est donc probablement une source DAC valide mais **muette** sur cette carte (« plain DAC=4 is silent » dans la référence).
3. Les noms de `ak_ioctls.h` côté capture (`SET_STATE(2)`, `SET_PERIODS(2)`) sont probablement « source 2 » et « gain micro 2 ». **Ne pas toucher à la séquence de capture** : elle marche par rejeu fidèle.
4. **L'ampli (GPIO 69) est piloté par le pilote lui-même** : propriétés device-tree `speak-gpios` / `speak-gpios-en` dans `ak_pcm.ko`, appliquées par `IOC_SET_SPEAKER`. Le firmware d'origine confirme : `/usr/local/_ht_hw_settings.ini` `[audio]` `speak_en_gpio = 69`, `speak_gpio_effective_level = 1` ; `anyka_ipc` journalise « enbale/disable audio speaker », `ht_audio_codec_ao_speaker(%d)`, `AoSetSrcAndSpk`, `dac ioctl IOC_SET_SPEAKER failed`. La référence note aussi « 69 = speaker amp enable, also driven by the audio driver ». **Aucun script du firmware (`sbin/*.sh`, `main.sh`, `service.sh`) ne touche le GPIO 69** (seuls Wi-Fi et capteur de température). Donc : **pas de sysfs GPIO 69 côté `ak_rtsp`** (le pilote l'a déjà réservé : `export` échouerait en `EBUSY`, et on risquerait un double pilotage).
5. Volume d'origine : `_ht_sw_settings.ini` `speak_vol = 100`, `mic_vol = 100` (côté `anyka_ipc`, via sa bibliothèque AO/VQE, que la référence dit inutilisable hors `anyka_ipc` : écho/VQE `_SD_Echo_GetDacStream … not true` → clic sans son). On contourne la bibliothèque, comme `aoplay`.
6. Le firmware d'origine décode du **PCM µ-law** (`sub_decode_type = 18`) et du MP3 (`main_decode_type = 2`) pour son propre « talk » : G.711 µ-law est donc aussi le codec natif de la plateforme.
7. Fichiers sonores d'origine : `/usr/share/*.mp3` (`dingdong.mp3`, `ding_ding_ding.mp3`, `sdcard_*.mp3`, invites chinoises…), lus par `anyka_ipc` (décodeur interne). **Propriétaires, jamais redistribués ni utilisés** : le carillon Akam est synthétisé (§6.2). `bin/beep` n'est qu'un applet BusyBox (haut-parleur PC via evdev) sans rapport.
8. ~~Format côté DAC~~ (remplacé par §1.3 : mono, 512 trames de 64 ms) : la référence exigeait du **stéréo entrelacé s16le** ; la période de lecture d'origine capturée est **1024 octets** (= 256 trames stéréo = 32 ms à 8 kHz, même cadence que la capture). On convertira donc mono → stéréo (L = R) et on écrira des périodes de 1024 octets. La référence utilise `period_bytes=16384 × 16` (512 ms par période : inacceptable pour du push-to-talk).

### 1.3 Capture du firmware d'origine (2026-11-10, vérité terrain)

Une capture `LD_PRELOAD` de l'application d'origine sur une caméra d'origine (outil `docs/stock_capture.md` ; les journaux bruts sont propriétaires et **ne sont pas dans le dépôt**) a levé les hypothèses de §1.2. Premier essai matériel d'Akam : `PREPARE` + `RESUME` fonctionnent (pas de plantage noyau), mais `SET_SPEAKER 0x80045081` renvoyait `EINVAL`, l'ampli ne s'allumait jamais, rien n'était audible.

- **Codage des directions : asm-generic standard.** `0x4004xxxx` = `_IOW` (SET, `copy_from_user`), `0x8004xxxx` = `_IOR` (GET, la valeur est renvoyée). Les anciens alias de lecture de `ak_ioctls.h` étaient à l'envers : `0x80045010` et `0x80045030` sont des **GET** (stock les appelle et lit 0 / 2), les SET sont **`0x40045010`** (`SET_SOURCES`) et **`0x40045030`** (`SET_GAIN`), et **`SET_SPEAKER = 0x40045081`**. Le GET du débit est `0x800450ea` (renvoie 8000). Les anciens noms côté capture ne changent pas (la capture rejoue des octets qui marchent).
- **Séquence d'ouverture stock** (fd `/dev/pcmC0D0p`, `O_WRONLY`) : `SET_SOURCES = 4`, `SET_SPEAKER = 1`, `PREPARE {format 0, 8000 Hz, 1 canal (MONO), 16 bits, period_bytes 1024, periods 16}`, `RESUME` (arg 0), lecture du débit (`0x800450ea` -> 8000).
- **Chaque invite jouée** : `GET_SOURCES`, `SET_SOURCES = 4`, `SET_SPEAKER = 1`, `SET_GAIN = 2` (une fois 3), écritures de PCM mono s16le 8 kHz, puis `SET_SOURCES = 0` à la fin (**route coupée au repos**). `SET_SPEAKER = 0` n'a jamais été observé (14 s de capture).
- **Source 4 = route de lecture audible** sur cette carte. L'ancien commentaire « 4 = DAC muet, 6 = DAC+LINEIN audible » était faux/non vérifié. 6 reste proposé comme réglage d'expérience.
- **Format** : **mono**, période de 1024 octets = 512 trames = **64 ms**, 16 périodes pilote (même format que la boucle d'écho `/dev/pcmC0D0l`). La conversion mono -> stéréo prévue en §1.2 point 8 est abandonnée.

## 2. Architecture

```
PC : ffmpeg (micro -> µ-law 8 kHz) -- TCP 8092 --> talk.c ──push──> audio_jitter (ring d'octets G.711, 400 ms max)
                                                                         │  (SPSC, sans verrou, RING_BARRIER comme audio.c)
                                         audio_out.c (thread lecture, seul écrivain du fd) <── tirage 1 période (512 trames)
                                           décode G.711 -> s16 -> volume -> write(fd, 1024 o, mono)
                                           silence si le tampon est vide (le flux frère reste actif)
8091  SET audio.out_*  ──> audio_out_set_*()     (volume, gain, source, enable, test)
```

- `audio_out.[ch]` **remplace** `audio_playback_loop` quand `audio.out_enable=1` (**valeur par défaut**, décision utilisateur) ; avec `enable=0`, ou si la garde anti-boucle ou la séquence matérielle échouent, le chemin actuel (silence, séquence actuelle) reste **strictement inchangé**.
- `talk.[ch]` : un thread `poll()` unique (accept + un seul client), jamais un thread par client.
- `g711.[ch]`, `audio_jitter.[ch]` : code pur sans matériel ni socket, testable sur l'hôte.
- Les en-têtes `audio_out.h`, `audio_jitter.h`, `g711.h` sont figés par la première tâche qui les écrit. API de référence (figée par T2) : `size_t audio_out_push(const int16_t *pcm, size_t n_samples)` (PCM mono 8 kHz déjà décodé par `talk.c` via `g711.h` ; appelée par `talk.c`, jamais bloquante ; passe par le tampon de gigue `audio_jitter`, retourne `n_samples`), `void audio_out_set_talker(const char *ip_or_null)`, `int audio_out_mic_muted(void)` (consulté par le chemin d'envoi du micro, §4.3).
- RAM : ring 3,2 Ko (400 ms de G.711) + 1 période (1 Ko) + pile du thread ≈ < 40 Ko. CPU : décodage par table de 256 entrées, négligeable (8 000 octets/s).

### 2.1 Séquence matérielle (identique à celle de l'application d'origine, §1.3)

Au démarrage, **si `audio.out_enable=1`**, et seulement après le démarrage de la capture (comme aujourd'hui) :

```
open("/dev/pcmC0D0p", O_WRONLY)
IOC_SET_SOURCES  <- audio.out_source   (4 par défaut ; 0 si audio.out_amp=off)   (0x40045010)
IOC_SET_SPEAKER  <- 1                  (0 si audio.out_amp=off)                  (0x40045081)
IOC_PREPARE      <- { id=0, rate=8000, channels=1, bits=16, period_bytes=1024, periods=16 }   (0xE0)
IOC_RESUME                              (0xE1, jamais appelé avant un PREPARE réussi)
GET_RATE                                (0x800450ea, doit renvoyer 8000 ; journalisé sinon)
IOC_SET_GAIN     <- audio.out_gain      (2 par défaut)                           (0x40045030)
write(fd, 1024 octets) en boucle
```

- **Règle de sûreté (test hôte obligatoire, §9)** : `RESUME` n'est jamais émis si `PREPARE` n'a pas réussi ; en cas d'échec d'une étape, on referme le fd et on retombe sur le chemin actuel, journalisé.
- `periods=16` (≈ 1 s de tampon pilote) est la valeur du firmware d'origine ; le pilote valide `period_bytes % 64 == 0` et `periods` ∈ [1, 0x50]. L'écriture bloque tant que le tampon pilote est plein, la cadence est donc celle du DAC (plancher de pas : 90 % de 64 ms).
- **Garde anti-boucle de reboot** : une panique noyau au démarrage est le pire cas (la caméra redémarre `ak_rtsp` et replante). Avant la nouvelle séquence, `ak_rtsp` écrit le marqueur `/etc/config/akam/audio_out.trying` ; il l'efface après 5 s de lecture sans erreur. Si le marqueur existe déjà au démarrage : la séquence est **sautée**, `audio.out_state=disabled_crash` et un message est journalisé ; `akam_audio_out.sh clear-guard` (ou `./akam talk --reset-guard`) le retire. Une écriture jffs2 par démarrage avec `enable=1` : acceptable.
- Ne jamais fermer ni rouvrir le fd de lecture pendant le fonctionnement pour couper le son (le flux frère doit rester actif) : on coupe la **route et l'ampli** (`SET_SOURCES 0`, `SET_SPEAKER 0`) et on continue d'écrire du silence.

## 3. Étape A — preuve de lecture locale

### 3.1 Livrable

Sans réseau : un bip de 1 s généré dans le code (sinus 1 kHz, 8 kHz, rampes de 10 ms, amplitude −16 dBFS) déclenché par la commande 8091 :

```
printf 'SET audio.out_test 1\n' | nc <cam> 8091      # joue le bip puis revient à 0
```

`audio.out_test` fonctionne dès que `audio.out_enable=1` (défaut). Aucun fichier son, aucun outil sur la caméra. Variantes de mise au point **en direct, sans reflasher** : `audio.out_source` (4 ou 6), `audio.out_gain` (0..5), `audio.out_volume` (0..100), `audio.out_amp` (`auto`/`on`/`off`, pour mesurer le souffle).

Raisonnement : une seule image flashée suffit pour toute la mise au point (source, gain, périodes), c'est le coût réel du matériel ici.

### 3.2 Politique de l'ampli (GPIO 69, via le pilote)

- `audio.out_amp=auto` (défaut) : comme le firmware d'origine, la route et l'ampli sont allumés à l'ouverture, puis **coupés au repos** : 2 s après l'ouverture sans son, ou **2 s après le dernier octet audible** (maintien, pour ne pas enchaîner clics et pops entre deux phrases). Allumage à l'arrivée du premier octet d'une session (parleur, bip, carillon) : `SET_SOURCES <source>`, `SET_SPEAKER 1`, `SET_GAIN <gain>`, puis une pré-charge d'une période de silence (64 ms, >= 60 ms), puis audio. Extinction : `SET_SPEAKER 0` puis `SET_SOURCES 0`. `SET_SPEAKER 0` n'a jamais été vu dans la capture stock : s'il échoue, l'échec est journalisé **une fois** et n'est pas rejoué à chaque période (la route coupée suffit à rendre muet).
- `audio.out_amp=on` garde route et ampli allumés en permanence ; `off` les garde éteints (ouverture avec `SET_SOURCES 0`, `SET_SPEAKER 0`, aucun gain émis).
- Risques d'un ampli laissé allumé en permanence : souffle audible en pièce calme (le bruit de fond de l'ampli, à mesurer), consommation et échauffement de la carte ajoutés (faibles sur un ampli de petite puissance mais non nuls dans un boîtier fermé), usure du haut-parleur sans intérêt. D'où `auto` ; `on` n'existe que pour la mesure.
- Extinction obligatoire sur tous les chemins : arrêt normal (`audio_stop`), signal, échec d'écriture, désactivation par `SET audio.out_enable 0`. Si `ak_rtsp` est tué par `kill -9`, la fermeture du fd par le noyau doit couper l'ampli (« close hp channel ») : **à vérifier** (T6, point B10).
- `IOC_SET_SPEAKER` échoue (`EINVAL`/`ENOTTY`/autre) : journaliser, **ne pas** basculer sur sysfs GPIO 69 (§1.2 point 4). Diagnostic manuel possible : `cat /sys/kernel/debug/gpio` (si debugfs) pour voir l'état de la ligne 69.

### 3.3 Test matériel A (l'utilisateur, par SSH ; critères à l'oreille)

| # | Action | Attendu |
|---|---|---|
| A1 | Image avec `audio.out_enable=1` ; `dmesg`/journal série au démarrage | séquence §2.1 sans erreur ; pas de panique ; marqueur `audio_out.trying` effacé après 5 s ; vidéo et micro RTSP normaux |
| A2 | `SET audio.out_volume 30`, `SET audio.out_gain 2`, `SET audio.out_test 1` | un bip net de 1 s (sinon : voir A3) |
| A3 | Si silence ou clic seul : vérifier les lignes `[audio_out]` (aucun `failed`), monter `SET audio.out_gain` ; en dernier recours `SET audio.out_source 6` (expérience) puis rejouer | audible avec la source 4 et le gain 2 d'origine (capture stock) |
| A4 | Gains 0, 1, 2, 3, 4, 5 à volume 100 | monotone croissant ; noter le niveau « confortable » (sert au défaut D8) ; pas de saturation/craquement |
| A5 | Volume 100, 50, 25, 10, 0 à gain fixe | monotone ; 0 = silence complet ; pas de pop notable à l'allumage/extinction de l'ampli |
| A6 | `SET audio.out_amp on`, oreille près du haut-parleur en pièce calme, puis `auto` | mesurer le souffle d'ampli au repos (justifie `auto`) ; avec `auto`, silence total entre deux bips |
| A7 | Écoute RTSP (ffplay) pendant un bip | le bip est entendu avec le retard RTSP (écho acoustique, §6) ; fps vidéo inchangé |
| A8 | 20 bips de suite, `top`, `free` | pas de fuite mémoire, CPU de `ak_rtsp` inchangé à +1 % près, aucune erreur `[audio] playback write` |
| A9 | Même test avec `audio.out_enable=0` (image précédente) | comportement strictement identique à aujourd'hui |

## 4. Étape B — entrée réseau : comparaison (REMPLACÉ par le rétrocanal ONVIF, spec 07 §1 : le port 8092 n'est pas construit)

| Option | Verdict |
|---|---|
| **TCP brut sur un port dédié (8092), G.711, une ligne d'en-tête** | **Retenue.** Sans dépendance, ~250 lignes de C, testable hôte, un seul client, authentification calquée sur la spec 04, trivial à piloter depuis bash (`/dev/tcp`), `ffmpeg`, `nc`, et depuis go2rtc (`exec:`, §5.3). |
| Rétrocanal RTSP / ONVIF Profile T (`Require: www.onvif.org/ver20/backchannel`, piste SDP `a=sendonly`, RTP entrant sur les trames `$` interleaved) | **Reportée à l'étape D.** Seul standard que Frigate/go2rtc/NVR reconnaissent nativement pour « parler », et l'auth Digest existe déjà. Mais : il faut étendre SmolRTSP (SDP, `SETUP` d'une piste entrante, dépaquetage RTP) et `rtsp_in.c` (qui jette aujourd'hui les trames `$` entrantes comme du RTCP) : risque sur le chemin vidéo qui marche, hors règle « ne pas réécrire ce qui marche ». Réutilisera la même API `audio_out_push()`. Faisabilité à confirmer avant de s'y engager. |
| HTTP POST (flux chunked) | Écartée : analyseur HTTP + chunked côté caméra pour aucun client existant (les navigateurs exigent HTTPS pour le micro, de toute façon traité par go2rtc). |
| RTP/UDP brut | Écartée : pas d'authentification, pas de verrou de locuteur, pertes sans bénéfice sur un LAN Wi-Fi à 8 ko/s. |
| Texte du port 8091 | Écartée : flux binaire dans un protocole ligne à ligne partagé avec les journaux. |
| Tube nommé + SSH (`ssh cam 'cat > /tmp/talk'`) | Écartée comme chemin principal (démarrage SSH ≈ 1 s sur ARM926, pas de verrou). **Chiffrement possible avec la solution retenue** : `ssh -N -L 8092:127.0.0.1:8092 root@cam` (Dropbear est déjà là). |

### 4.1 Protocole du port 8092 (v1) (REMPLACÉ, spec 07 §1.2)

Écoute `INADDR_ANY` **seulement si `audio.out_enable=1`** (la valeur par défaut ; avec `enable=0` le port est fermé, aucune écoute). `listen` avec backlog 1.

```
-> TALK <codec> [client=<jeton ≤32 car. A-Za-z0-9._-/>] LF          codec = pcmu | pcma | l16   (8000 Hz, mono ; l16 = s16 petit-boutiste)
<- CHALLENGE <nonce>  LF                                            seulement si l'auth du port 8091 est ON (spec 04 §9) ; nonce = 32 hexa
-> AUTH <mac> LF                                                    mac = HMAC-SHA256(K, "akam-talk-v1:" || nonce_hex) ; même clé K que la spec 04
<- OK talking <codec> prefill=120 LF
-> <octets du flux, sans trame, jusqu'à la fermeture ou la pause>
```

- En-tête : ligne ≤ 128 octets, délai 3 s, sinon `ERR syntax` et fermeture. Erreurs : `ERR <code> [texte]` puis fermeture ; codes : `syntax`, `codec`, `busy`, `auth_required`/`auth_failed` (messages indifférenciés), `disabled`.
- **Verrou de locuteur unique** : le premier connecté garde le haut-parleur. Un second reçoit `ERR busy` immédiatement (pas d'éviction : un locuteur en cours n'est pas coupé par un inconnu). Un locuteur muet > 3 s est libéré (`idle`), fermeture.
- Pas de trame : G.711 = 1 octet par échantillon, rien à aligner ; `l16` garde l'octet impair en attente.
- Authentification : **même modèle opt-in que la spec 04** (états OFF/ON/BROKEN du fichier `ctl_auth`, échec fermé sur BROKEN). OFF = ouvert sur le LAN ; ON = HMAC. La limitation de débit des échecs (spec 04 §9.5) est partagée. Le préfixe `akam-talk-v1:` interdit qu'un MAC du port 8091 serve ici et inversement.
- Aucun secret dans les journaux ; journal : `[talk] <ip> "akam-cli" pcmu started/ended`, compteurs.

### 4.2 Tampon de gigue, latence, dérive

- Préremplissage **120 ms** (960 octets) avant le premier tirage ; si le tampon se vide (sous-alimentation) : silence, compteur `underruns`, retour à l'état de préremplissage. Plafond **400 ms** : au-delà, on jette les plus anciens jusqu'à 120 ms (compteur `drops`) ; c'est aussi la correction de dérive d'horloge (± 100 ppm = 1 ms/10 s : négligeable pour des tirades de quelques minutes).
- Latence bouche-haut-parleur estimée : capture PC 20-40 ms + réseau Wi-Fi 5-30 ms + 120 ms + tampon pilote (≈ 2 à 3 périodes de 64 ms) ≈ **250-350 ms** : acceptable pour un push-to-talk, pas pour une conversation. À mesurer (T6).
- Ces constantes (`JITTER_PREFILL_MS`, `JITTER_MAX_MS`) sont des `#define`, pas des réglages utilisateur.

### 4.3 Half-duplex et écho

Le micro de la caméra capte son propre haut-parleur. Deux effets : (1) le spectateur RTSP entend sa propre voix retardée de ~1 s (écho de bout en bout) ; (2) un retour de boucle (Larsen) est possible si le PC joue l'audio de la caméra près de son micro. Il n'y a **pas** d'annulation d'écho (hors périmètre). Retenu : **half-duplex** : pendant qu'un locuteur est actif (et 300 ms après), `audio_get_frame_ts()` renvoie du **silence** au lieu du micro (`audio.out_mute_mic=1` par défaut). Les paquets RTP audio continuent donc de partir avec les bons horodatages (le lecteur reste synchronisé), mais plus d'écho ni de Larsen. `0` laisse le micro ouvert (pour tester l'écho à l'oreille).

## 5. Étape C — côté PC (REMPLACÉ, spec 07 §1.5)

### 5.1 Commandes `./akam` (dans `akam` et `tools/wizard/**`, mêmes conventions que `rtsp-auth`)

```
./akam talk [--camera NOM|IP] [--codec pcmu|pcma] [--ptt] [--source <périphérique>]   micro -> caméra
./akam talk --stdin [--codec pcmu|pcma]                  lit le G.711 sur l'entrée standard (intégration go2rtc)
./akam say <fichier.wav|mp3|…>                           ffmpeg convertit en G.711 8 kHz et envoie
./akam talk --test                                       envoie SET audio.out_test 1 (bip, étape A)
./akam talk --volume N | --gain N | --enable | --disable | --status | --reset-guard
```

- Mise en œuvre bash pure, sans `nc` : `exec 3<>/dev/tcp/$ip/8092`, ligne `TALK`, lecture de la réponse avec délai, HMAC calculé par la bibliothèque hôte de la spec 04 (T9) quand `CHALLENGE` arrive, puis `ffmpeg … >&3`.
- Capture : Linux `ffmpeg -f pulse -i default` (repli `-f alsa -i default`) ; macOS `-f avfoundation -i ":0"` ; filtres `-ac 1 -ar 8000 -af highpass=f=100,alimiter` (limiteur contre la saturation) `-c:a pcm_mulaw -f mulaw -`. Windows : hors périmètre (WSL sans micro).
- `--ptt` : **bascule** Entrée = parler / Entrée = couper (un terminal n'a pas d'événement de relâchement de touche). Sans `--ptt` : parler jusqu'à Ctrl-C. À l'ouverture, message : « le micro de la caméra est coupé pendant que vous parlez ».
- Le PC ne mélange rien : `ffmpeg` absent → message clair et sortie ≠ 0 (comme `doctor`).
- `akam say` : plafonne à 60 s par défaut ; `--force` au-delà.
- `--enable/--disable/--volume/--gain` passent par le client 8091 de la spec 04 (avec l'auth HMAC si activée) et par `akam_audio_out.sh` (§7) pour la persistance.

### 5.2 Verrou et erreurs côté PC

`ERR busy` : « quelqu'un parle déjà (<client=…>) » ; connexion refusée : « sortie audio désactivée sur la caméra (`./akam talk --enable`) ».

### 5.3 Intégration go2rtc / Home Assistant (plus tard, ne bloque pas V1.1)

go2rtc propose le « two-way audio » pour ses sources `rtsp` (ONVIF Profile T), `exec`, `webrtc`, etc. ; le navigateur envoie le micro en WebRTC (HTTPS obligatoire pour le micro) en PCMU/PCMA/Opus. La source **`exec` avec `#backchannel=1`** écrit du **PCMA** (ou PCM 48 kHz) sur l'entrée standard de la commande. Pont sans code caméra supplémentaire :

```yaml
streams:
  akam_cam:
    - rtsp://<user>:<pass>@<cam>:554/…                                   # vidéo + micro
    - exec:akam talk --stdin --codec pcma --camera <cam>#backchannel=1#audio=alaw/8000   # exemple d'après la doc go2rtc : à valider
```

C'est pourquoi `pcma` est dans le protocole (go2rtc fournit de l'A-law). Les cartes Home Assistant (WebRTC Camera, go2rtc intégré) s'appuient sur ce flux ; à valider en T6. Le jour où l'étape D (rétrocanal RTSP/ONVIF) existe, la source `rtsp` de go2rtc fera la même chose directement, sans `exec`.

### 5.4 Étape D : rétrocanal RTSP/ONVIF (devenu l'étape 1, spec 07 §1)

Au moins : `DESCRIBE` avec `Require: www.onvif.org/ver20/backchannel` renvoie une piste audio `a=sendonly` PCMU/8000 ; `SETUP` de la piste ; RTP entrant (interleaved `$`) dépaqueté vers `audio_out_push()`. Prérequis : étude de faisabilité SmolRTSP + `rtsp_in.c` ; non engagée par cette spec.

## 6. Volume, activation, carillon

### 6.1 Clés 8091 (nouvelles ; grammaire/auth de la spec 04, écritures authentifiées quand l'auth est ON)

| Clé | Type / bornes | Défaut | Persistant (§7) |
|---|---|---|---|
| `audio.out_enable` | 0/1 | **1** (décision utilisateur) | oui |
| `audio.out_volume` | int 0..100 (logiciel, courbe quadratique : `(v/100)²` en Q15) ; 0 = muet (ampli éteint) | 50 | oui |
| `audio.out_gain` | int 0..5 (matériel, `IOC_SET_GAIN`) | 2 | oui |
| `audio.out_source` | enum 4 / 6 (route ; 4 = stock, 6 = expérience) | 4 | oui |
| `audio.out_amp` | enum `auto`/`on`/`off` | `auto` | non |
| `audio.out_mute_mic` | 0/1 (§4.3) | 1 | oui |
| `audio.out_chime` | 0/1 carillon au démarrage (§6.2) | **0** | oui |
| `audio.out_test` | 1 = joue le bip (déclencheur, repasse à 0) | 0 | non |
| `audio.out_state` (lecture seule) | `off`/`idle`/`talking`/`test`/`chime`/`disabled_crash`/`error` | | |
| `audio.out_client` (lecture seule) | IP du locuteur ou `none` | | |
| `audio.out_underruns`, `audio.out_drops` (lecture seule) | uint (depuis le démarrage) | | |
| `audio.out_buf_ms` (lecture seule) | uint, remplissage instantané | | |

Sans la spec 04 fusionnée, ces clés s'ajoutent à la grammaire v1 actuelle de `control.c` (`SET clé valeur` → `OK clé=valeur`) sans toucher aux clés existantes. Valeurs hors bornes : `ERR` comme les clés actuelles.

Changement de `audio.out_volume`/`gain`/`source` : effet à la période suivante (pas de redémarrage, pas de pop : le volume logiciel est lissé sur 10 ms).
`SET audio.out_enable 1` à chaud ne ré-ouvre pas le périphérique : il est pris en compte **au prochain démarrage** (`ak_rtsp` relancé par le watchdog ou reboot) ; la réponse l'indique (`OK audio.out_enable=1 (restart to apply)`). C'est volontaire : pas de ré-initialisation du pilote en cours de vie.

### 6.2 Carillon au démarrage (désactivé par défaut)

Deux sons synthétisés (880 Hz 120 ms puis 1175 Hz 160 ms, rampes 10 ms, −18 dBFS) joués **une fois**, quand le serveur RTSP écoute, si `audio.out_chime=1`. Utile pour savoir à l'oreille que la caméra a fini de démarrer. Mêmes règles d'ampli (§3.2). Jamais les mp3 d'origine.

## 7. Persistance et activation

- Version autonome (sans spec 04 fusionnée) : fichier `/etc/config/akam/audio_out`, `0600`, dossier `0700`, lignes `clé=valeur` (`enable`, `volume`, `gain`, `source`, `mute_mic`, `chime`), écriture atomique tmp+`mv`, lu au démarrage. Absent = tous les défauts, donc **sortie audio activée** (décision utilisateur ; `enable=0` dans le fichier la désactive). Géré par `/usr/sbin/akam_audio_out.sh` (POSIX sh, BusyBox 1.30.1, sans `tr` ni `mktemp`) : `status | enable | disable | set <clé> <valeur> | clear-guard`.
- Quand le magasin `ctl_store` de la spec 04 existe : les clés `audio.out_*` persistantes y migrent (`SAVE`) et le fichier devient une migration à la première lecture ; une seule source de vérité (décision D4).
- La sortie audio étant activée par défaut, le wizard n'a rien à activer ; `./akam talk --disable` (`enable=0`) la coupe, `--enable` la rétablit.

## 8. Sécurité (port 8092 REMPLACÉ ; modèle de sécurité de la parole : spec 07 §1.6)

- LAN uniquement ; jamais de redirection de port/UPnP vers 8092 (README). Pas de TLS : écoute passive possible (le flux sonore est audible en clair) ; chiffrement : tunnel SSH (§4 dernière ligne).
- **Activé par défaut** (décision utilisateur) : le port 8092 est ouvert dès que la sortie audio l'est, mais exige toujours l'auth HMAC (décision « Auth talk » en bas) : sans clé provisionnée, aucune session n'est acceptée. Avec `enable=0`, aucun port ouvert. Le README le dit sans détour. MAC par session de parole, nonce à usage unique, échecs limités comme 04 §9.5.
- Un seul locuteur, durée de session non bornée tant que des données arrivent (idle 3 s) : un client verrouille la caméra tant qu'il parle ; accepté (déni de service LAN possible, comme 03/04). Volume maximal par défaut 50 : pas de cri surprise.
- Le port ne lit que 128 octets d'en-tête puis des octets bruts décodés par table : aucune allocation dynamique, aucun analyseur complexe. Fuzz hôte du décodeur d'en-tête (§9).
- Les journaux ne contiennent ni audio ni secret.

## 9. Plan de test

### 9.1 Hôte (`tests/*_test.sh` appelant du C natif, comme `rtsp_in_test.c`)

- **G.711** : vecteurs ITU (µ-law : `0xFF` → 0, `0x7F` → −0, `0x80` → +32124, `0x00` → −32124 ; A-law : `0xD5` → +8, `0x55` → −8) comparés à une table de référence générée une fois par `ffmpeg`/`sox` et figée dans `tests/fixtures/` ; aller-retour encode/décode avec erreur ≤ pas de quantification ; les 256 codes décodés ; `l16` avec octets impairs.
- **Tampon de gigue** : préremplissage, tirage périodique, sous-alimentation (silence + compteur + ré-amorçage), dépassement (jet des plus anciens jusqu'à 120 ms), arrivée par paquets de 1 à 1400 octets, taille exactement une période, séquence aléatoire à graine fixe contre un modèle naïf, aucun accès hors bornes (compilation `-fsanitize=address,undefined` en natif).
- **Protocole `talk`** (via `socketpair`/boucle locale, sans caméra) : en-tête valide ×3 codecs ; codec inconnu, ligne trop longue, pas de LF dans les 3 s ; `busy` pour un second client et libération sur idle/fermeture ; `auth` OFF/ON/BROKEN, MAC juste/faux/rejoué/nonce périmé, `ct_equal`, limitation d'échecs ; flux qui s'arrête en cours d'octet impair.
- **Périphérique factice** (`audio_out` compilé avec un `struct dev_ops` à pointeurs de fonction : `open/ioctl/write/close`) qui **enregistre la séquence d'appels** et injecte des échecs : (a) ordre stock `SET_SOURCES`, `SET_SPEAKER`, `PREPARE`, `RESUME`, `GET_RATE` ; (b) **`RESUME` jamais appelé si `PREPARE` a échoué** ; (c) `SET_SPEAKER 0` et `SET_SOURCES 0` sur chaque chemin de sortie (arrêt, échec d'`ioctl`, échec d'écriture) ; (d) écriture toujours de 1024 octets, mono, silence quand le tampon est vide ; (e) maintien d'ampli 2 s simulé avec une horloge injectée, une session dans les 2 s ne bascule pas l'ampli ; (f) garde `audio_out.trying` : présente au démarrage → séquence sautée.
- **Volume** : 0 = silence exact, 100 = identité, courbe monotone, pas de dépassement 16 bits (saturation), lissage 10 ms sans pop.
- **Contrôle** (`control.c`) : golden de la grammaire v1 inchangé ; clés `audio.out_*` : bornes, `enable` « restart to apply », `out_test` retombe à 0.
- **CLI** (`tests/talk_test.sh`) : fausse caméra TCP (script) vérifiant le trafic exact (`TALK pcmu …`, octets µ-law d'un fichier connu, HMAC correct avec la clé de test), `ERR busy`, port fermé, `ffmpeg` absent, `--stdin`, `shellcheck` propre.
- Bash/shell : `tools/ci/local_check.sh` vert ; aucun changement du comportement v1 de `control.c`.

### 9.2 Matériel (étape A déjà en §3.3 ; B/C ci-dessous ; chaque point est une inconnue à lever)

| # | Point | Critère |
|---|---|---|
| B1 | Port 8092 ouvert par défaut (`enable=1`) ; fermé avec `enable=0` | `nc -zv` |
| B2 | `./akam talk` 30 s au micro du PC | voix intelligible, pas de saccades (underruns ≈ 0 dans `audio.out_underruns`), latence ≈ 0,3 s (battement de mains, enregistrement du PC si possible) |
| B3 | Wi-Fi faible (éloigner la caméra) | coupures gérées sans claquement, `drops`/`underruns` croissent, reprise propre |
| B4 | Deux terminaux `talk` en même temps | le second reçoit `ERR busy` ; l'autre continue |
| B5 | Coupure brutale du PC (câble réseau, Ctrl-C, `kill -9`) | silence en ≤ 3 s, ampli éteint après 2 s, locuteur libéré |
| B6 | Auth HMAC ON | sans clé : refus ; avec clé : parole ; MAC rejoué refusé |
| B7 | Écoute RTSP pendant l'émission | micro muet (`mute_mic=1`) ; vidéo inchangée (fps via 8091) ; pas d'écho/Larsen ; `mute_mic=0` : écho audible (preuve de l'intérêt) |
| B8 | `akam say fichier.wav` (10 s, 5 min avec `--force`) | lecture complète sans glitch ; mémoire stable |
| B9 | 1 h d'inactivité puis parole ; boucle 100 sessions courtes | pas de fuite, pas de pop répété, `ak_rtsp` stable, pas de reboot |
| B10 | `SET audio.out_enable 0` + restart, `kill -9 ak_rtsp` | retour au comportement actuel ; ampli éteint après kill (souffle absent) |
| B11 | Carillon (`chime=1`) | joué une fois, pas à chaque reconnexion RTSP |
| B12 | go2rtc `exec:` + carte WebRTC (optionnel) | parole du navigateur/téléphone au haut-parleur |

## 10. Découpage en tâches (un agent DEV par tâche ; ≤ 2 en parallèle sur fichiers disjoints)

Chaque tâche fige les en-têtes de ses modules (relus à la fusion). Les ajouts au `Makefile` sont d'une ligne par fichier. Aucun agent n'agit sur la caméra ; tout est testé sur l'hôte avec périphérique factice.

| Vague | Tâche | Fichiers | Dépend de |
|---|---|---|---|
| 1 | **T1** G.711 (µ-law, A-law, l16) + tampon de gigue + tests | `src/ak_rtsp/g711.[ch]`, `audio_jitter.[ch]`, `tests/g711_test.sh`, `tests/jitter_test.sh`, `tests/native/g711_test.c`, `jitter_test.c`, fixtures | — |
| 1 | **T5 (REMPLACÉE, spec 07 T4/T5)** CLI `./akam talk|say` + bibliothèque PC + fausse caméra | `akam`, `tools/wizard/**`, `tests/talk_test.sh`, `tests/fixtures/fake_talk_cam.sh` | spec 04 T9 (HMAC hôte) ou stub |
| 2 | **T2** `audio_out` : séquence périphérique (§2.1), garde, ampli (§3.2), volume, bip, thread de lecture remplaçant le silence, clés 8091 d'étape A (`enable`, `volume`, `gain`, `source`, `amp`, `test`, `state`), câblage `main.c`, correction des noms dans `ak_ioctls.h` (alias, sans changer les valeurs ni la séquence de capture) | `src/ak_rtsp/audio_out.[ch]`, `audio.c`, `audio.h`, `ak_ioctls.h`, `control.c`, `main.c`, `Makefile`, `tests/audio_out_test.sh`, `tests/native/audio_out_test.c` | T1 |
| 2 | **T3 (REMPLACÉE, spec 07 T3)** serveur `talk` : port 8092, en-tête, verrou, auth (module 04 ou, à défaut, repli « auth OFF » + point d'accroche), poussée dans le tampon, `audio.out_client/underruns/drops/buf_ms` | `src/ak_rtsp/talk.[ch]`, `tests/talk_proto_test.sh`, `tests/native/talk_test.c` | T1 (API `audio_out_push` figée dès T1) |
| 3 | **T4** persistance et activation : `akam_audio_out.sh`, patch `build_firmware.sh`/`image_files.txt`, carillon, `audio.out_mute_mic` dans `audio_get_frame_ts`, intégration wizard (`verify`), README (port 8092, risque LAN) | `tools/firmware_patch_templates/akam_audio_out.sh`, `src/ak_rtsp/build_firmware.sh`, `audio.c` (muet micro), `README.md`, `docs/**`, `CHANGELOG.md`, tests | T2, T3 |
| 4 | **T6** validation matériel A (§3.3) puis B/C (§9.2), correctifs ciblés (source, gain, périodes, `period_bytes`) ; confirmation des hypothèses de §1.2 | rapport + correctifs | T2 (A), tout (B) |
| opt. | **T7** go2rtc/Home Assistant (§5.3) et étude de faisabilité de l'étape D | `docs/` | T6 |

**Point d'arrêt recommandé : après T2, l'utilisateur fait l'étape A** (une image, §3.3). Si A échoue (silence, instabilité noyau), on ajuste (T6) avant d'investir dans T3-T5. T1 et T5 n'attendent pas : pur hôte.

## 11. Décisions ouvertes (recommandations)

- **D1 — Activation.** **Tranché par l'utilisateur (2026-11-10) : activée par défaut** (`enable=1`, fichier absent = activé), port 8092 ouvert mais toujours protégé par l'auth HMAC. Alternative écartée : `enable=0` par défaut avec proposition du wizard.
- **D2 — Politique d'ampli.** Recommandé : `auto`, maintien 2 s, jamais allumé au repos. À arbitrer par la mesure A6.
- **D3 — Protocole.** Recommandé : TCP 8092 + G.711 (µ-law, A-law, l16) ; rétrocanal RTSP/ONVIF en étape D. Alternative : commencer directement par ONVIF (plus de clients natifs, plus de risque sur le cœur RTSP).
- **D4 — Persistance.** Recommandé : `ctl_store` de la spec 04 si fusionné avant T4 (clés `audio.out_*`), sinon le fichier `akam/audio_out` décrit en §7.
- **D5 — Micro coupé pendant la parole.** Recommandé : oui (`mute_mic=1`), seule parade à l'écho sans AEC.
- **D6 — Port.** `8092` proposé (à vérifier libre dans l'image : `netstat -tln`). Alternative : réutiliser 8091 avec une commande `TALK` qui bascule la connexion en mode binaire (un port de moins, mais mélange le texte et le binaire et casse le clivage « 8091 = réglages »).
- **D7 — Format DAC.** **Mono** 16 bits 8 kHz, périodes 1024 octets × 16 (64 ms), comme le firmware d'origine (capture, §1.3). Remplace l'ancienne décision « stéréo 1024 × 8 ».
- **D8 — Défauts de niveau.** Volume 50, **gain 2 et source 4 (valeurs du firmware d'origine)**, à ajuster après A4/A5 (le but : audible mais jamais agressif).
- **D9 — Carillon.** Synthétisé, désactivé par défaut, jamais les mp3 d'origine.
- **D10 — Entrée SSH/FIFO.** Écartée (tunnel SSH `-L 8092` suffit pour le chiffrement) ; à rouvrir si l'authentification HMAC paraît trop lourde.
- **D11 — Source des constantes matérielles.** Les noms/valeurs de `ak_ioctls.h` côté lecture sont corrigés par alias (`AKPCM_IOC_PREPARE`, `RESUME`, `SET_SOURCES` `0x40045010`, `SET_GAIN` `0x40045030`, `SET_SPEAKER` `0x40045081`, `GET_RATE`, plus les GET `0x8004...`), **numéros confirmés par la capture du firmware d'origine (§1.3)** ; confirmation par Ghidra de `ak_pcm.ko` (qui a déjà servi pour la capture) en parallèle de T2 si l'utilisateur le souhaite.

---

## Décisions issues de la capture stock (2026-11-10) — priment sur ce qui précède

- Lecture **mono** 8 kHz 16 bits, périodes de 1024 octets (512 trames, 64 ms), 16 périodes pilote.
- **Source 4** par défaut (route audible), gain 2 ; 6 conservé comme expérience ; 0 = route coupée (interne, repos).
- Numéros d'ioctl de lecture pris sur la capture : `SET_SOURCES 0x40045010`, `SET_GAIN 0x40045030`, `SET_SPEAKER 0x40045081`, `GET_RATE 0x800450ea`.
- Séquence d'ouverture comme le firmware d'origine ; ampli `auto` : allumage au premier son, route (`SET_SOURCES 0`) et ampli (`SET_SPEAKER 0`) coupés après 2 s de silence.

## Décisions utilisateur (2026-11-10) — font foi

- **Sortie audio ACTIVÉE par défaut** (`audio.out_enable=1`), port talk 8092 ouvert ; l'ampli ne s'allume que pendant une session (mode `auto`, extinction 2 s après) ; l'auth suivra le modèle opt-in.
- Autres décisions ouvertes : recommandations de la spec retenues (port 8092 dédié ; persistance dans `/etc/config/akam/audio_out` en attendant `ctl_store` de la spec 04 ; micro coupé pendant la parole ; défauts volume/gain fixés après l'étape A à l'oreille ; carillon désactivé ; rétrocanal ONVIF/RTSP plus tard ; alias pour les noms d'ioctl).
- Ordre : T2 en priorité (étape A, bip à l'oreille) en parallèle de T1 ; T3/T5 après validation de l'étape A.
- **Auth talk (2026-11-10, décision utilisateur, option 2)** : le port talk 8092 exige TOUJOURS une authentification HMAC-SHA256 (challenge-response de la spec 04). **Une seule clé par caméra**, partagée avec les écritures du port 8091 : `/etc/config/akam/ctl_key` (256 bits, générée par le wizard à l'installation/au `register`, gardée côté PC par caméra). Pour le 8091, l'exigence de la clé sur les écritures reste opt-in (spec 04 D1) ; pour le 8092 elle est obligatoire. Sans clé provisionnée, le serveur talk refuse toute session (log explicite) — jamais d'ouverture sans auth. Le module HMAC/SHA-256 (spec 04 T1) devient un prérequis de T3.
