# Akam — Spécification : API de contrôle 8091 v2 (lecture, configuration, auth HMAC)

Statut : rédigée le 2026-11-09 (agent SPEC, branche `docs/spec-control-api`, partie de `dev`). Cible : V1.1. Seule cette spec est en français ; protocole, messages, code, README et docs sont en anglais.

> **Mise à jour du 2026-11-10 (spec 07, `specs/07-spec-standard-protocols.md`, décision utilisateur D1 étape 4) : le port 8091 devient un port de débogage et de métriques, en lecture seule par défaut.**
> - **Remplacé (ne pas implémenter)** : l'authentification HMAC-SHA256 (§0 décisions 2 et 3, §9 entier, §10 entier, `akam_ctl_auth.sh`, `./akam ctl-auth`, fichier `ctl_key`, §12 tests d'auth), les tâches **T1** (SHA-256/HMAC), **T5**, **T6** et la partie auth de **T9** (§14), les décisions D1, D5 et D7 (§15) et la ligne « `SET` en v1, auth ON » de §11. Les écritures ne sont plus protégées par un secret réseau : elles sont refusées à distance (`ERR read_only`), acceptées depuis la boucle locale (SSH) et, à distance, seulement quand le drapeau de débogage RAM est activé par SSH (spec 07 §4).
> - **Reste valable** : §1 à §8 (grammaire, clés lues §3, JSON §4, persistance `ctl_store` §5 utilisée par SSH/MQTT/ONVIF, nom d'hôte §6, connexions §7, erreurs §8), §11 pour les lectures, et les tâches T2, T3, T4, T7 (sans auth), T8, T10, T11. Nouveau code d'erreur : `read_only`.
> - Les clés lisibles du §3.1 sont les métriques de la spec 07 §4.2.

## 0. Décisions actées (utilisateur)

- Le port 8091 devient une vraie API de lecture et de configuration, **compatible** avec l'existant : `LIST` / `GET` / `SET` ne changent pas pour un client qui ne connaît pas la v2.
- **Lectures ouvertes** sur le LAN. **Écritures authentifiées** quand l'authentification est activée.
- Authentification **HMAC-SHA256 défi-réponse** (le secret ne circule jamais), **optionnelle**, gérée par une commande caméra et par `./akam`, secret stocké dans CONFIG.
- Protocole et docs en anglais.

Hors périmètre V1.1 : TLS, gestion du Wi-Fi/IP statique par 8091, mise à jour du firmware, changement du mot de passe RTSP par 8091 (reste interdit, spec 03).

## 1. Constats sur l'existant (control.c, 401 lignes)

- Texte, une ligne par commande, `\r` ignoré, ligne > 255 octets **tronquée sans erreur** puis exécutée. Commandes insensibles à la casse (`strcasecmp`), clés sensibles.
- Réponses : `LIST` = `clé=valeur` par ligne puis `.` ; `GET` = `clé=valeur` nu ; `SET` = `OK clé=valeur` ; erreur = `ERR texte libre`. Les valeurs entières passent par `atoi`/`strtoul` : `SET ae.speed abc` donne 0 sans erreur.
- Un seul client ; une nouvelle connexion **remplace** l'ancienne (le client évincé est fermé). `listen(…, 1)`.
- Lignes `LOG <texte>` asynchrones, envoyées à l'unique client à tout moment, y compris **au milieu d'un bloc `LIST`** (chaque ligne est verrouillée, pas le bloc). `dprintf` bloquant sous le verrou : un client lent peut figer le tee de stdout, donc tout le firmware (risque existant, corrigé ici, §7).
- Réglages volatils (perdus au reboot). `venc.minqp/maxqp` affichés `28 (read-only)` (suffixe dans la valeur).
- Écoute sur `INADDR_ANY`, pas d'authentification, pas de délai d'inactivité.

## 2. Grammaire et versionnement

### 2.1 Lexique

```
line      = command *( SP arg ) LF         ; CR ignoré ; 1 à 255 octets hors LF (v2 : plus long = ERR too_long, ligne jetée)
command   = 1*ALPHA / "AUTH-CHALLENGE"    ; insensible à la casse
key       = group "." name ; group = 1*(a-z), name = 1*(a-z / 0-9 / "_")   ; sensible à la casse, <= 32 octets
value     = reste de la ligne après "SET key SP", espaces de tête/queue retirés (v2) ; octets 0x20-0x7E ; <= 128 octets
reply     = ok / err / kv / block
ok        = "OK" [SP texte] LF
err       = "ERR" SP code [SP texte] LF           ; v2 ; un client v1 ne regarde que le préfixe "ERR"
kv        = key "=" value LF                      ; réponse de GET
block     = *kv "." LF                            ; LIST, STATUS, HELP : se termine par la ligne "." seule
log       = "LOG" SP texte LF                     ; asynchrone, voir §7
```

- Un client sait ce qu'il lit d'après la commande envoyée : `LIST`, `STATUS` (texte), `HELP` répondent par un bloc ; tout le reste par une seule ligne ; `* json` par une seule ligne JSON. Dans un bloc, aucune ligne de données ne commence par `.`, `ERR ` ou `LOG ` (les clés commencent par une minuscule suivie de `.` ; `LOG ` est en majuscules). Une erreur sur une commande bloc est une ligne `ERR …` **sans** `.`.
- Valeurs affichées : octets hors 0x20-0x7E et `\` échappés en `\xNN` / `\\` (ex. un SSID avec un octet 0xE9). Un client lit donc toujours de l'ASCII imprimable.
- Mode **v1 (legacy)** : tout client qui n'envoie pas `HELLO` avant sa première autre commande. Réponses, messages d'erreur (`ERR unknown param X`, …), `venc.minqp=28 (read-only)`, lecture tolérante (`atoi`) et lignes `LOG` non sollicitées sont **exactement** ceux d'aujourd'hui. Seules exceptions (corrections de défauts, au CHANGELOG) : ligne > 255 octets refusée au lieu d'être tronquée ; écriture sans session quand l'auth est activée (§9.4).
- Mode **v2** : après `HELLO`. Erreurs `ERR <code> <texte>`, parsing strict (`SET ae.speed abc` = `ERR invalid`), pas de `LOG` sauf `SUBSCRIBE logs`, pas de suffixe `(read-only)`.

### 2.2 Versionnement et capacités

```
-> HELLO [client=<jeton ≤32 car. A-Za-z0-9._-/>] [api=<n>]
<- OK akam-ctl api=2 min_api=1 fw=<git> auth=off|on|broken caps=status,json,info,save,reset,auth,logs,hostname
```

- `api` = version de grammaire du serveur (2). Elle n'augmente que pour un changement **incompatible**. Nouvelles clés, commandes, capacités : additifs, sans changement de `api` ; un client teste `caps` (et gère `ERR unknown_cmd`).
- Un client qui annonce `api=N` > 2 reçoit quand même `api=2` et décide. `client=` ne sert qu'au journal (`[control] client 192.168.1.5 "akam-cli"`), jamais à une décision d'accès.
- `HELLO` est accepté une seule fois, avant toute autre commande de configuration ; un second donne `ERR invalid`. `PING` → `OK pong`. `QUIT` → `OK bye` puis fermeture. `HELP` → bloc : une commande par ligne.

### 2.3 Commandes

| Commande | Accès | Réponse |
|---|---|---|
| `HELLO`, `PING`, `QUIT`, `HELP` | ouvert | voir §2.2 |
| `LIST [json]` | ouvert | réglages ; v1 : bloc des clés d'aujourd'hui, inchangé ; v2 : + `sys.hostname` |
| `GET <key>` | ouvert | `key=value` (toute clé de §3, y compris en v1 pour les nouvelles clés) |
| `STATUS [json]` | ouvert | tout l'état (§3) en bloc texte ou en JSON |
| `INFO <key>` | ouvert (v2) | `OK key type=int min=-50 max=50 rw=1 persist=1 default=0` |
| `SET <key> <value>` | **auth si ON** | `OK key=value` (valeur normalisée, relue) |
| `SAVE` | **auth si ON** | `OK saved keys=<n> bytes=<b>` ou `OK unchanged` |
| `RESET [persist]` | **auth si ON** | `OK reset` / `OK reset persist` |
| `AUTH-CHALLENGE`, `AUTH <hex>` | ouvert | §9 |
| `SUBSCRIBE logs`, `UNSUBSCRIBE logs` | ouvert (v2) | `OK subscribed logs` |

`STATUS` sans `HELLO` (client v1) est accepté et répond comme en v2 texte.

## 3. Clés

Types : `int`, `uint`, `str`, `enum`. « Sans info » : la valeur est `unknown` en texte, `null` en JSON (jamais une valeur inventée, jamais une erreur). Les clés existantes gardent noms, bornes et sémantique (`ae.*`, `night.*`, `isp.*`, `venc.*`).

### 3.1 Lecture seule (nouvelles)

| Clé | Type | Source / calcul |
|---|---|---|
| `fw.version` | str | `GIT_HASH` de `version_info.h` (`git describe`) |
| `fw.build` | uint | `BUILD_NUMBER` ; `fw.built` str = `BUILD_TIMESTAMP` |
| `fw.api` | uint | 2 |
| `sys.uptime` | uint s | 1er champ de `/proc/uptime`, partie entière |
| `sys.load` | str | 3 premiers champs de `/proc/loadavg` (`0.12 0.10 0.05`) ; JSON : tableau de 3 nombres |
| `sys.cpu` | uint % | occupation CPU 0-100, voir 3.3 |
| `sys.mem_total` | uint kB | `MemTotal` de `/proc/meminfo` |
| `sys.mem_free` | uint kB | `MemAvailable` s'il existe (noyau 4.4 : oui), sinon `MemFree+Buffers+Cached` |
| `sys.flash_total`, `sys.flash_free` | uint octets | `statvfs("/etc/config")` : `f_blocks*f_frsize`, `f_bavail*f_frsize` (CONFIG jffs2 256 KiB ; valeur jffs2 estimative, à valider §12.2) |
| `net.ip` | str | `ioctl(SIOCGIFADDR)` sur `wlan0` (puis `eth0` si `wlan0` n'a pas d'adresse) |
| `net.mac` | str | `ioctl(SIOCGIFHWADDR)`, minuscules `aa:bb:cc:dd:ee:ff` |
| `net.ssid` | str | voir 3.4 |
| `net.wifi_rssi` | int dBm | voir 3.4 |
| `video.resolution` | str | `ENC_WIDTH`x`ENC_HEIGHT` (`1920x1080`) |
| `video.fps` | uint | fréquence nominale `ENC_FPS` |
| `video.fps_actual` | uint ×10 | images/s mesurées sur les 5 dernières s (compteur d'images dans `rtsp.c`, dixièmes : `199` = 19,9) ; `unknown` hors PLAY |
| `video.bitrate` | uint kbit/s | octets vidéo RTP émis sur les 5 dernières s ×8/5/1000 ; `0` hors PLAY |
| `rtsp.state` | enum | `idle` (aucun client) / `connected` (avant PLAY) / `playing` |
| `rtsp.client` | str | IP du client RTSP courant ou `none` (jamais de nom d'utilisateur ni d'URI) |
| `rtsp.since` | uint s | secondes depuis le dernier changement de `rtsp.state` (`CLOCK_MONOTONIC`) |
| `rtsp.auth` | enum | `off` / `on` / `broken` (état de `rtsp_auth_db`, sans le nom d'utilisateur) |
| `night.state` | enum | existant (`day`/`night`) |
| `night.hw_exp` | uint | exposition matérielle courante de `night.c` (ajoutée seulement si `night.c` l'expose déjà) |
| `ctl.api`, `ctl.auth` | uint, enum | version d'API ; `off`/`on`/`broken` de l'authentification 8091 |
| `ctl.clients` | uint | connexions ouvertes sur 8091 |
| `ctl.unsaved` | uint | 1 si des réglages vivants diffèrent de ce que `SAVE` écrirait (§5) |
| `venc.minqp`, `venc.maxqp` | uint | existants, lecture seule (28 / 43) |

### 3.2 Inscriptibles

| Clé | Type / bornes | Persistant |
|---|---|---|
| `ae.stable_range`, `ae.hold_range`, `ae.speed`, `ae.exp_max` | int (bornes en v2 lues dans `ae.c`, exposées par `INFO` ; comportement v1 inchangé) | oui |
| `ae.enabled` | 0/1 | oui |
| `night.mode` | enum `auto`/`day`/`night` | oui |
| `night.trigger_hw_exp`, `night.day_hw_exp`, `night.confirm_samples`, `night.lock_ms` | uint/int | oui |
| `isp.saturation`, `isp.contrast`, `isp.brightness`, `isp.sharpness` | int -50..50 | oui |
| `sys.hostname` | str, RFC 1123 (§6) | oui |

### 3.3 `sys.cpu`

Calcul sans thread dédié : à chaque collecte on lit la ligne `cpu` de `/proc/stat`, `busy = total - idle - iowait`, `cpu% = 100 * Δbusy / Δtotal` depuis l'échantillon précédent mémorisé. Si l'échantillon précédent a plus de 30 s, ou n'existe pas, on prend deux échantillons espacés de 250 ms (`usleep`, une seule fois, borné) et on répond sur cette fenêtre. Si `Δtotal = 0`, valeur du cache. La valeur est donc « la moyenne depuis la lecture précédente » (entre 1 s et 30 s selon la cadence de l'appelant) ; documenté ainsi. Arrondi à l'entier.

Le résultat de **toute** la collecte `STATUS` est mis en cache 1 s : un sondage à 20 Hz coûte une collecte par seconde (~8 lectures de fichiers + 1 `statvfs` + 2 `ioctl`).

### 3.4 Wi-Fi : ce qui est vérifié et ce qui ne l'est pas

- L'APP stock contient `iwconfig`, `iwlist`, `iwpriv` (`app/bin/`) : le pilote `atbm6x3x` expose donc au moins l'interface wireless-extensions. Le BusyBox 1.30.1 n'a pas `iw`.
- **Sources retenues (sans `fork`)** : `net.wifi_rssi` = colonne `level` de la ligne `wlan0` de `/proc/net/wireless` (si le fichier existe ; valeur déjà en dBm quand elle finit par `.`), sinon `ioctl(SIOCGIWSTATS)`, sinon `unknown`. `net.ssid` = `ioctl(SIOCGIWESSID)` sur `wlan0` ; en repli seulement, le SSID **configuré** dans `/etc/config/wpa_supplicant.conf` (`ssid="…"`, sans jamais lire ni exposer `psk`) ; sinon `unknown`. Pas de `popen("iwconfig")` : un `fork` d'un processus sous charge vidéo sur 64 Mo coûte plus que ces lectures.
- **À VÉRIFIER sur matériel** (T11) : lequel de ces chemins répond avec ce pilote. Le code les tente tous dans cet ordre et n'échoue jamais ; l'API est identique quel que soit le résultat.
- Confidentialité : `net.ssid`, `net.ip`, `net.mac`, `rtsp.client` sont lisibles sans authentification, par décision. Aucune clé n'expose un mot de passe, un `psk`, le HA1 ni le secret HMAC.

## 4. Sortie JSON

Commande : `STATUS json` ou `LIST json` (mot-clé final `json`, insensible à la casse). Réponse : **une seule ligne** UTF-8 terminée par `\n`, sans `.` final. Aucun mode de connexion à mémoriser (`nc` en une commande fonctionne).

```json
{"api":2,"fw":{"version":"v1.1.0-3-gabc123","build":42,"built":"2026-11-09 12:00:00","api":2},"sys":{"hostname":"akam","uptime":86400,"load":[0.12,0.10,0.05],"cpu":18,"mem_total":57344,"mem_free":21000,"flash_total":262144,"flash_free":170000},"net":{"ip":"192.168.1.50","mac":"aa:bb:cc:dd:ee:ff","ssid":"home","wifi_rssi":-58},"video":{"resolution":"1920x1080","fps":20,"fps_actual":199,"bitrate":2100},"rtsp":{"state":"playing","client":"192.168.1.10","since":120,"auth":"off"},"night":{"mode":"auto","state":"day"},"isp":{"saturation":0},"ae":{"enabled":1},"ctl":{"api":2,"auth":"on","clients":2,"unsaved":0}}
```

(Abrégé : le vrai objet contient toutes les clés de §3.) Règles :

- Un objet par groupe (partie avant le premier `.`), les membres sont les parties après ; ordre de §3, stable mais sans garantie contractuelle.
- Types : `int`/`uint` → nombre JSON ; `str`/`enum` → chaîne ; `sys.load` → tableau de 3 nombres à 2 décimales ; valeur sans info → `null`. Aucun autre flottant.
- Échappement (RFC 8259) : `"` → `\"`, `\` → `\\`, octets < 0x20 → `\u00XX`, `/` non échappé. Octets ≥ 0x80 : copiés s'ils forment de l'UTF-8 valide (séquences ≤ 4 octets, sans forme trop longue), sinon remplacés par `?` (un SSID peut contenir n'importe quels octets ; le JSON reste valide).
- Taille : tampon statique de **2048 octets** (sortie nominale ~700 ; marge pour des chaînes de 128 octets). Débordement : `ERR internal json overflow`, jamais de JSON tronqué. `LIST json` (réglages seuls) tient en 600 octets.
- `LIST` texte (v2) et `STATUS` texte : mêmes clés, en `clé=valeur`, bloc terminé par `.`, maximum 4096 octets, écrit en un seul `send` (§7).

## 5. Persistance, `SAVE`, `RESET`

- Fichier : `/etc/config/akam/settings` (CONFIG jffs2, `0600`, répertoire `0700`). Texte :

```
# akam settings v1
ae.speed=40
night.mode=night
sys.hostname=cam-salon
#end
```

- Seules les clés **différentes du défaut compilé** y figurent (fichier typique < 300 octets, plafond 2 KiB ; au-delà `ERR too_large`). Un changement de défaut dans un firmware ultérieur profite donc à qui n'a rien réglé. L'en-tête et le trailer `#end` sont obligatoires (détection de troncature) ; un fichier sans eux est ignoré en entier et journalisé.
- **Écriture uniquement sur `SAVE`** (jamais sur `SET`). Algorithme : sérialiser en RAM ; si le contenu est **octet pour octet identique** au fichier actuel (ou fichier absent et rien à écrire) → aucune écriture, `OK unchanged` ; sinon écrire `settings.new.<pid>` (`0600`), `fsync`, `rename` sur `settings` (atomique sur jffs2), `fsync` du répertoire. Plus rien à enregistrer et fichier présent : `unlink`. Un lecteur voit l'ancien ou le nouveau fichier, jamais un mélange.
- **Usure** : jffs2 sur 256 KiB. Un `SAVE` coûte un nœud de données (≲ 300 o), un nœud d'inode et la collecte de déchets associée. Limites : un `SAVE` effectif au plus toutes les **10 s** et **20 par heure** (`ERR busy retry=<s>` au-delà) ; aucun `SAVE` implicite, aucune écriture périodique, aucun journal sur CONFIG.
- **Chargement** : `main.c` appelle `control_settings_load()` **avant** de démarrer `ae`/`night` et avant la première application ISP, avec les mêmes validateurs que `SET`. Clé inconnue ou valeur invalide : ligne ignorée et journalisée (`[control] settings: ignored "key"`), jamais un échec de démarrage.
- `RESET` : remet tous les réglages **en RAM** aux défauts compilés (nom d'hôte noyau compris, §6). `RESET persist` : idem + supprime le fichier `settings`. `ctl.unsaved` vaut 1 dès qu'un réglage vivant diffère du contenu que `SAVE` écrirait.
- `settings` ne contient jamais de secret ; l'état d'authentification 8091 est dans un fichier à part (§9).

## 6. `sys.hostname` et DHCP

### 6.1 Validation (RFC 1123 stricte, une seule étiquette)

1 à 63 caractères `A-Za-z0-9-`, ni début ni fin par `-`, pas de point, pas uniquement des chiffres (évite l'ambiguïté avec une adresse IP). Stocké et renvoyé **en minuscules**. Sinon `ERR invalid hostname`. Défaut compilé : `IPCamera` (valeur envoyée aujourd'hui ; voir D2).

### 6.2 Constats : comment le nom atteint le DHCP

- Le nom est passé par `udhcpc -i wlan0 -x hostname:IPCamera` à **deux** endroits : `ak_rtsp_wrapper.sh` (notre script) et `sbin/ethernet.sh` du stock (`get_ip_from_ap`, ligne 120). L'option est figée dans les arguments du processus : un renouvellement de bail la renvoie à l'identique.
- Relancer `udhcpc` à chaud n'est **pas sûr** : les scripts stock font `killall udhcpc`, et le script `udhcpc` par défaut exécute `deconfig` au démarrage (adresse retirée : coupure du flux et de la connexion 8091 ; perte d'accès si le DHCP échoue). Décision : **pas de relance à chaud**.
- `/etc/hostname` : racine en squashfs lecture seule, inutilisable. Le nom persistant vit dans `/etc/config/akam/settings` (`sys.hostname=`).

### 6.3 Comportement retenu

1. `SET sys.hostname X` : validation, puis `sethostname(X)` immédiat (visible par `sys.hostname`) ; réponse `OK sys.hostname=x` (v2 : ajoute `note=dhcp_next_boot`). Le routeur verra le nouveau nom **au prochain démarrage** (nouveau `DISCOVER`).
2. `SAVE` écrit le nom dans `settings`.
3. Au démarrage : **`/usr/sbin/akam_hostname.sh get`** (POSIX sh, `sed` seul ; sans `tr`/`mktemp`) lit `sys.hostname=` dans `settings`, revalide (même règle), sinon affiche `IPCamera`. `ak_rtsp_wrapper.sh` l'utilise pour `hostname "$H"` et `udhcpc … -x hostname:"$H"` ; `build_firmware.sh` remplace `-x hostname:IPCamera` par `-x hostname:"$(/usr/sbin/akam_hostname.sh get)"` dans `ethernet.sh` (patch `sed` comme `patch_service_sh.sh`, échec bruyant si la ligne est introuvable).
4. `./akam` peut proposer `reboot` après un changement de nom (jamais automatique).
5. Pas de mDNS/avahi dans l'image : le nom n'est résolu que par le DHCP/DNS du routeur.

## 7. Connexions, concurrence, journaux, débit

### 7.1 Clients simultanés

Aujourd'hui un sondeur d'état éjecterait l'outil de réglage, et inversement. Décision : **jusqu'à 4 connexions** (`CTL_MAX_CLIENTS`), servies par **un seul thread `poll()`** (le thread d'acceptation actuel), pas un thread par client.

- RAM : 4 × (tampon de ligne 256 o + état ~64 o) + un tampon de réponse partagé de 4 KiB, environ 5 KiB. Un thread par client coûterait au moins 4 piles (32 à 64 Kio chacune) pour un trafic minuscule.
- Rien ne bloque le thread : pas de `sleep` (le délai anti-brute-force est un `not_before` par client, §9.5 ; l'échantillon CPU de 250 ms est le seul blocage toléré, borné), sockets non bloquantes, `send(MSG_DONTWAIT|MSG_NOSIGNAL)` ; un client dont la file d'envoi est pleine est fermé.
- Quand les 4 places sont prises, la nouvelle connexion **évince le client le plus inactif** (généralise « la nouvelle remplace l'ancienne » : un client v1 seul se comporte exactement comme avant).
- Délai d'inactivité : **10 min** pour un client v2 sans `SUBSCRIBE` ; **aucun** pour un client v1 (l'onglet de réglage reste ouvert des heures) et pour un abonné aux journaux.
- `listen` backlog 4 ; au plus 10 `accept` par seconde (au-delà, fermeture immédiate) pour qu'une boucle de reconnexion ne monopolise pas le CPU.

### 7.2 Journaux (`LOG`)

- Client v1 : reçoit les `LOG <ligne>` comme aujourd'hui.
- Client v2 : **rien** tant qu'il n'a pas envoyé `SUBSCRIBE logs` ; `UNSUBSCRIBE logs` l'arrête. Les lignes restent préfixées de `LOG ` et ne sont jamais émises au milieu d'une réponse : toute réponse (ligne ou bloc) est composée en RAM puis envoyée par **un seul** `send` sous le verrou client, que le pump de journaux utilise aussi. Un client démultiplexe sur le préfixe `LOG `.
- Texte filtré : octets de contrôle remplacés par `?`, 400 caractères maximum.
- Client lent : envoi non bloquant ; en cas d'`EAGAIN` la ligne est **abandonnée** (compteur) et une ligne `LOG [dropped N lines]` est émise au retour de la place. Le tee vers la console série ne dépend jamais d'un client réseau.
- Les journaux restent ouverts (comme aujourd'hui) ; ils peuvent contenir des IP de clients mais, par la spec 03, jamais de secret.

### 7.3 Limites de débit

- Par connexion : seau à jetons, 20 commandes/s en continu, rafale 40 ; au-delà `ERR busy retry=1`, commande non exécutée.
- Écritures matérielles (`SET isp.*`) : 20/s au plus (les `isp_set_*` font un GET-modify-SET vers le matériel).
- `SAVE` : §5. `AUTH` : §9.5. Ligne > 255 octets : `ERR too_long`, reste de la ligne jeté.

## 8. Codes d'erreur (v2 : `ERR <code> [texte]`)

| Code | Signification |
|---|---|
| `syntax` | ligne mal formée, argument manquant |
| `unknown_cmd` | commande inconnue |
| `unknown_key` | clé inconnue |
| `read_only` | clé en lecture seule |
| `invalid` | valeur non analysable (texte : attendu) |
| `range` | valeur hors bornes (texte : bornes) |
| `too_long` | ligne ou valeur trop longue |
| `too_large` | fichier de réglages dépasserait 2 KiB |
| `auth_required` | écriture sans session authentifiée alors que l'auth est ON |
| `auth_failed` | réponse HMAC fausse (message identique pour tous les motifs) |
| `auth_broken` | fichier d'auth présent mais invalide : écritures refusées (échec fermé) |
| `auth_disabled` | `AUTH-CHALLENGE`/`AUTH` alors que l'auth est OFF |
| `busy` | limite de débit, backoff ou `SAVE` trop fréquent (`retry=<s>`) |
| `io` | lecture/écriture CONFIG ou matériel en échec |
| `internal` | bug (dépassement de tampon JSON…) |

En v1 les messages restent ceux d'aujourd'hui (sans code) ; seule ligne nouvelle : `ERR auth required (use AUTH-CHALLENGE then AUTH)`.

## 9. Authentification HMAC-SHA256 (REMPLACÉ par la spec 07 §4 : non implémenté)

### 9.1 Principe

Clé partagée `K` (32 octets aléatoires). Le serveur envoie un nonce ; le client répond `HMAC-SHA256(K, message)`. `K` ne circule jamais. Seules les **écritures** (`SET`, `SAVE`, `RESET`) exigent une session authentifiée ; lectures, `HELLO`, `SUBSCRIBE` restent ouverts.

### 9.2 Fichier et états

`/etc/config/akam/ctl_auth` (`0600`, dossier `0700`, une ligne, écriture atomique tmp+`mv`) :

```
akam-ctl-1:<64 hexa minuscules>      K en hexadécimal
```

Même modèle d'états que la spec 03 : **absent = OFF** (comportement actuel), **valide = ON**, **présent mais illisible/invalide = BROKEN** (échec fermé : lectures ouvertes, toute écriture refusée `auth_broken`, journalisé ; `akam_ctl_auth.sh set|disable` répare). Le fichier est relu au `HELLO`, à chaque `AUTH-CHALLENGE` et avant chaque écriture (≈ 70 octets, pas de redémarrage). Changement de clé ou désactivation : toutes les sessions authentifiées sont invalidées immédiatement (la session retient un condensé de la clé qui l'a ouverte, comparé à celle du fichier).

Le secret équivaut à un mot de passe pour qui lit CONFIG (comme le HA1 de la spec 03) : fichier `0600`, jamais exposé par 8091, jamais journalisé.

### 9.3 Échange

```
-> AUTH-CHALLENGE
<- OK <nonce>        nonce = 16 octets de getrandom() (repli /dev/urandom ; échec = ERR internal, fermé), 32 hexa minuscules
-> AUTH <mac>        mac = 64 hexa (minuscules ou majuscules acceptées)
<- OK authenticated
```

- `mac = HMAC-SHA256(K, "akam-ctl-v1:" || nonce_hex)` où `nonce_hex` est la chaîne ASCII de 32 caractères renvoyée (pas les octets bruts) et `K` les 32 octets décodés. Le préfixe de contexte évite la réutilisation d'un MAC dans un autre protocole utilisant la même clé.
- Le nonce est **à usage unique** : consommé par le premier `AUTH` qui suit, **même en cas d'échec** ; il expire après 30 s ; un nouveau `AUTH-CHALLENGE` remplace l'ancien. `AUTH` sans défi en cours : `ERR syntax`. Un MAC rejoué échoue (nonce inconnu).
- Session : drapeau `authed` **par connexion**, effacé à la déconnexion, au changement/retrait de la clé, et après **10 min** sans écriture ni nouvelle `AUTH` (le client refait un défi). Jamais partagée entre connexions.
- Comparaison du MAC en **temps constant** (XOR cumulatif sur les 32 octets, sans sortie anticipée) ; le MAC attendu est calculé même si la longueur ou l'hexadécimal reçu est invalide.
- Message d'erreur unique `auth_failed` (pas de distinction entre « mal formé » et « faux »).

### 9.4 Écritures hors session

Avec l'auth ON, un `SET`/`SAVE`/`RESET` sans session : `ERR auth_required` (v2) ou `ERR auth required (use AUTH-CHALLENGE then AUTH)` (v1) ; **aucune** modification. Avec l'auth OFF : comportement d'aujourd'hui.

### 9.5 Échecs et limitation

Compteur global au processus, remis à zéro après 60 s sans échec ; un échec = `AUTH` avec MAC faux ou mal formé (ni `AUTH-CHALLENGE`, ni un `SET` refusé). `not_before` par client (aucun blocage du thread) : échecs 1-2 → aucun délai ; 3 → 500 ms ; 4 → 1 s ; ≥ 5 → 2 s avant la réponse suivante de la connexion. 5 échecs sur une connexion : fermée. 10 échecs en 60 s globalement : tout `AUTH` reçoit `ERR busy retry=30` pendant 30 s (clients déjà authentifiés et lectures non affectés). Journal : `[control] auth: failed attempt N from <ip>` ; jamais le MAC, le nonce, la clé.

### 9.6 Limites assumées

- Pas de TLS : une écoute passive voit commandes et valeurs (les valeurs sont déjà ouvertes en lecture) mais pas `K`. Un attaquant **actif** sur le chemin peut détourner une session déjà authentifiée (injecter des `SET`). La session par connexion protège contre l'usage accidentel, les scripts curieux et les clients du LAN non autorisés, pas contre un homme du milieu (voir D5 : signature par commande, reportée). Contournement : tunnel SSH (§13).
- Sans l'auth, un client du LAN peut changer exposition, mode nuit, nom et faire un `SAVE` : c'est le comportement actuel (+ `SAVE`) ; d'où la commande d'activation.
- Dictionnaire hors ligne : `K` est aléatoire, non dérivé d'un mot de passe ; un enregistrement (nonce, MAC) ne s'y prête pas.
- Le déni de service reste possible (4 places, comme la spec 03).

### 9.7 HMAC-SHA256 : taille

`sha256.c` (domaine public ; FIPS 180-4 / RFC 2104) ~150 lignes, ~2,5 Ko de code ARM, état de 108 octets, sans allocation. API : `sha256_init/update/final`, `hmac_sha256(key,klen,msg,mlen,out32)`, `ct_equal(a,b,n)`. Vecteurs : FIPS 180-4 (`""`, `"abc"`, 448 bits, un million de `a`) et RFC 4231 cas 1-7.

## 10. Gestion de l'authentification (REMPLACÉ par la spec 07 §4 : non implémenté)

### 10.1 Commande caméra `/usr/sbin/akam_ctl_auth.sh` (POSIX sh, BusyBox 1.30.1, sans `tr` ni `mktemp`)

- `status` : `enabled (key id XXXXXXXX)` / `disabled` / `INVALID` (code retour 3). `key id` = 8 premiers hexa de `sha256("akam-ctl-id:"+K)` par `sha256sum` BusyBox (compare sans révéler `K`).
- `set` : génère `K` (`od -An -v -tx1 -N32 /dev/urandom`, blancs retirés par `sed`), l'installe (atomique, `0600`) et **l'affiche une seule fois** avec l'avertissement de le copier (usage direct en SSH interactif).
- `set-key` : lit UNE ligne `akam-ctl-1:<64 hexa>` sur stdin, la valide, l'installe (utilisée par `./akam` ; aucun secret dans `argv`).
- `disable` : supprime le fichier (écritures de nouveau ouvertes). `show` = `status`.
- Installée dans l'image (`tools/release/image_files.txt`) ; testée avec `AKAM_ROOT` comme `akam_rtsp_auth.sh`.

### 10.2 `./akam ctl-auth [set|import|disable|status]` (SSH avec la clé de l'utilisateur)

- `set` : génère `K` **sur le PC** (`openssl rand -hex 32`, sinon `od` sur `/dev/urandom`), l'écrit **d'abord** dans `${XDG_CONFIG_HOME:-~/.config}/akam/ctl_<hôte>.key` (`0600`, en `.new` puis renommée après succès de l'envoi pour ne pas perdre l'ancienne clé), l'envoie par stdin à `akam_ctl_auth.sh set-key`, puis vérifie par `status` (key id identique).
- `import` : lit une clé (stdin) obtenue par `akam_ctl_auth.sh set` sur la caméra et la range de même (second PC).
- `disable`, `status` : comme la caméra ; `status` compare le key id local et distant (`key matches` / `key differs`).
- Étape `verify` du wizard : après la question RTSP, « Protect the control port writes with a key? [y/N] » (défaut non ; sans terminal : non ; `--dry-run` : montrée, rien d'envoyé).
- `./akam ctl <STATUS|LIST|GET k|SET k v|SAVE|RESET>` (optionnel, T9) : client en ligne de commande : `HELLO`, authentifie avec la clé locale si `auth=on`, imprime la réponse. HMAC côté hôte : `openssl dgst -sha256 -mac HMAC -macopt hexkey:K`, sinon `perl -MDigest::SHA=hmac_sha256_hex`, sinon `python3 -I` (module `hmac`) ; échec explicite si aucun.
- `akam_hwcheck.sh` : ligne `Control API: api=2, auth enabled/disabled` (information, jamais un échec).

## 11. Compatibilité

| Client | Comportement |
|---|---|
| `printf 'LIST\n' \| nc -w 2 cam 8091` (v1) | inchangé (bloc + `.`), même sortie octet pour octet |
| `GET`/`SET` en v1, auth OFF | inchangé (lecture tolérante, `venc.minqp=28 (read-only)`) |
| outil de réglage graphique existant (v1, `LOG` non sollicité) | inchangé ; peut cohabiter avec un sondeur (4 places) |
| `SET` en v1, auth ON | `ERR auth required (...)` ; l'outil doit savoir faire `AUTH` (ancien outil : désactiver l'auth ou utiliser `./akam ctl`) |
| client v2 | `HELLO`, `STATUS json`, `SUBSCRIBE logs`, `AUTH` si besoin |
| `cam_ports_check` du wizard, `akam_hwcheck.sh` (port ouvert) | inchangés |
| nouvelles clés lues par un client v1 | accessibles par `GET`, absentes de `LIST` v1 (un parseur qui compte les lignes n'est pas perturbé) |

Rien ne change avec l'auth OFF, sauf les deux corrections du §2.1 (ligne trop longue ; `SAVE`, nouveau).

## 12. Plan de test

### 12.1 Hôte (`tests/*_test.sh` appelant du C natif comme `rtsp_auth_test.c`)

- `sha256`/HMAC : vecteurs FIPS 180-4 et RFC 4231 ; `ct_equal`.
- `ctl_sys` : lectures sur des **racines factices** (`/proc`, `/sys` simulés par répertoire, comme `AKAM_ROOT`) : `uptime`, `loadavg`, `meminfo` (avec et sans `MemAvailable`), `stat` (Δ=0, compteur qui recule), `net/wireless` (avec/sans point), `wpa_supplicant.conf` (SSID entre guillemets ; `psk` jamais lu), `statvfs` sur un dossier temporaire.
- `ctl_proto` avec backend factice : grammaire (lignes vides, CR, 255/256 octets, casse, valeurs avec espaces), `HELLO`/`PING`/`HELP`, v1 contre un fichier d'or (« golden » de la sortie actuelle de `LIST`/`GET`/`SET`/erreurs, **capturé avant** toute modification), v2 strict (`ae.speed abc`, bornes), `INFO`, blocs terminés par `.`, JSON (échappements, UTF-8 invalide, `null`, débordement, validité par `python3 -m json.tool` si disponible).
- Persistance : écriture atomique, contenu identique → aucune écriture (mtime inchangé), fichier tronqué/sans `#end`/clé inconnue/valeur invalide, plafond 2 KiB, limite `SAVE`, `RESET` / `RESET persist`.
- Nom d'hôte : valide/invalide (RFC 1123, longueurs 63/64, majuscules, tiret, chiffres seuls) ; `akam_hostname.sh get` sous BusyBox (pinné) avec fichier absent/corrompu.
- Auth : états OFF/ON/BROKEN, défi à usage unique, expiration 30 s, rejeu, MAC mal formé, temps constant (revue + test d'absence de sortie anticipée), backoff, 10 échecs globaux, invalidation à la rotation de clé, session 10 min ; `akam_ctl_auth.sh` sous BusyBox (`set`, `set-key`, `status`, `disable`, fichier corrompu, `AKAM_ROOT`).
- Intégration : serveur hôte (`ctl_proto` + sockets + backend factice, `tests/native/ctl_server_test.c`) et client scripté : 4 clients simultanés, éviction du plus inactif, client lent (journaux abandonnés, serveur jamais bloqué), journaux jamais mêlés aux blocs, inactivité v2 vs v1.
- Wizard : `ctl-auth` en `--dry-run` et contre `tools/wizard/fake_camera.sh` (étendue d'un serveur 8091 v2 minimal), clé jamais dans les journaux (sentinelle), échec d'envoi laissant l'ancienne clé, HMAC par les trois outils (comparaison croisée).
- Sentinelle : aucune sortie ni aucun journal ne contient la clé, un nonce d'échange échoué, ni un `psk`.

### 12.2 Matériel (T11 ; chaque point est une inconnue à lever)

`/proc/net/wireless` présent et lisible ; `SIOCGIWESSID`/`SIOCGIWSTATS` répondent avec `atbm6x3x` ; `iwconfig` présent dans l'image ; `MemAvailable` ; `statvfs("/etc/config")` raisonnable sur jffs2 ; `USER_HZ` = 100 ; coût d'un `STATUS` (≤ 5 ms, aucune perte d'images pendant 1 h de PLAY avec sondage à 10 Hz) ; `SAVE` répété : le second, identique, n'écrit rien (`flash_free` stable) ; redémarrage : réglages rechargés, nom visible dans la table de baux du routeur ; `SET sys.hostname` n'interrompt ni le flux ni le DHCP ; auth ON : écritures refusées sans clé, acceptées avec ; 4 clients + PLAY sans changement de fps ; RAM libre avant/après 1 h ; coupure de courant pendant `SAVE` (ancien ou nouveau fichier, jamais mixte). Tous les accès temps passent par `CLOCK_MONOTONIC` et le shim time32 (noyau 4.4), comme `rtsp_auth.c`.

## 13. Notes de sécurité

- Réseau local uniquement. Jamais de redirection de port, UPnP ou DMZ vers 8091 (README).
- Pas de TLS. Pour administrer depuis un réseau non fiable ou protéger une session d'un attaquant actif : tunnel SSH (Dropbear déjà présent) : `ssh -N -L 8091:127.0.0.1:8091 root@<cam>` puis le client sur `127.0.0.1:8091` ; ou en une commande `ssh root@<cam> 'printf "STATUS json\n" | nc -w 2 127.0.0.1 8091'`. Le port écoute sur toutes les interfaces (inchangé en V1.1 ; boucle locale seule : D8).
- Les lectures ouvertes divulguent SSID, IP, MAC, RSSI, nom, client RTSP, état de nuit : décidé, à indiquer au README. Aucune clé ne divulgue un secret.
- Le secret HMAC et le HA1 RTSP sont dans CONFIG, lisibles par root et par qui lit la flash : même niveau de confiance que les clés SSH déjà stockées.
- Aucun secret dans `argv`, journaux ou `STATUS`.

## 14. Découpage en tâches (un agent DEV par tâche ; ≤ 2 en parallèle sur fichiers disjoints)

Chaque tâche rédige et fige les en-têtes `ctl_*.h` de son module (relus à la fusion). Les ajouts au `Makefile` sont d'une ligne par fichier : conflits triviaux, résolus à la fusion.

| Vague | Tâche | Fichiers | Dépend de |
|---|---|---|---|
| 1 | **T1 (ABANDONNÉE, spec 07)** SHA-256 + HMAC + `ct_equal` + tests vecteurs | `src/ak_rtsp/sha256.[ch]`, `tests/sha256_test.sh`, `tests/native/sha256_test.c` | — |
| 1 | **T2** `ctl_sys` : lecteurs `/proc`, `statvfs`, `ioctl`, cache 1 s, CPU ; racine factice | `src/ak_rtsp/ctl_sys.[ch]`, tests + fixtures | — |
| 2 | **T3** `ctl_proto` : grammaire, `HELLO`/`INFO`/`HELP`, table de clés (types, bornes), erreurs, `STATUS`/`LIST` texte et JSON, framing, backend à pointeurs de fonctions ; golden v1 capturé d'abord | `src/ak_rtsp/ctl_proto.[ch]`, `ctl_json.[ch]`, tests | T2 |
| 2 | **T4** `ctl_store` (fichier `settings`, `SAVE`/`RESET`, chargement au boot, limite `SAVE`) + `akam_hostname.sh` + patch `ethernet.sh`/wrapper dans `build_firmware.sh` + `image_files.txt` | `src/ak_rtsp/ctl_store.[ch]`, `tools/firmware_patch_templates/akam_hostname.sh`, `ak_rtsp_wrapper.sh`, `src/ak_rtsp/build_firmware.sh`, tests | — |
| 3 | **T5 (ABANDONNÉE, spec 07)** `ctl_auth` : états, nonce, défi/réponse, backoff, session, relecture du fichier | `src/ak_rtsp/ctl_auth.[ch]`, tests | T1, T3 |
| 3 | **T6 (ABANDONNÉE, spec 07)** `akam_ctl_auth.sh` + test BusyBox + `image_files.txt` | `tools/firmware_patch_templates/akam_ctl_auth.sh`, `tests/ctl_auth_cmd_test.sh` | — |
| 4 | **T7** `control.c` : boucle `poll`, 4 places, éviction, v1/v2, `SUBSCRIBE`, `LOG` non bloquants, délais, `main.c` charge les réglages ; test d'intégration | `src/ak_rtsp/control.[ch]`, `main.c`, `tests/native/ctl_server_test.c` | T3, T4, T5 |
| 4 | **T8** compteurs RTSP (`stats` : état, client, fps, débit) branchés dans `rtsp.c`, exposition de `rtsp.auth` ; test hôte | `src/ak_rtsp/stats.[ch]`, `rtsp.c` | T2 |
| 5 | **T9** wizard : `./akam ctl-auth`, `ctl` optionnel, bibliothèque HMAC hôte, question dans `verify`, fausse caméra, ligne `hwcheck` | `tools/wizard/**`, `akam`, `tools/hw_tests/akam_hwcheck.sh`, tests | T6 |
| 5 | **T10** docs : référence du protocole (`docs/control_api.md`), README §8091, `docs/akam_v1_install.md` §4, `docs/troubleshooting.md`, CHANGELOG | `docs/**`, `README.md`, `CHANGELOG.md` | T7 |
| 6 | **T11** validation matériel (§12.2), correction des sources Wi-Fi / `flash_free` si besoin | rapport + correctifs ciblés | tout |

Chaque tâche : tests hôte verts (`tools/ci/local_check.sh`), shellcheck propre, aucun changement du comportement v1 (golden).

## 15. Décisions ouvertes (recommandations)

- **D1 — Auth par défaut.** Recommandé : **OFF par défaut** (rien dans l'image) ; le wizard la **propose** avec défaut « non » comme pour RTSP ; le README explique ce qu'un client du LAN peut faire sans elle. À reconsidérer en V1.2 (défaut « oui » quand le wizard installe).
- **D2 — Nom d'hôte par défaut.** Recommandé : conserver `IPCamera` (aucun changement visible chez les utilisateurs actuels) ; `akam` serait plus propre mais change la table de baux de tous.
- **D3 — Lecteurs concurrents.** Recommandé : un thread `poll()`, 4 places, éviction du plus inactif (§7.1), pas de threads supplémentaires.
- **D4 — Mode JSON.** Recommandé : suffixe `json` (`STATUS json`, `LIST json`), sans mode persistant. Alternative écartée : `FORMAT json` (état caché, mauvais pour `nc`).
- **D5 — Signature par commande.** Recommandé : **reporter** (V1.2) ; V1.1 = session par connexion. L'extension `W <seq> <mac> <cmd>` avec `mac = HMAC(K, session_nonce|seq|cmd)` s'ajoute par une capacité `signed` sans changer `api`.
- **D6 — Journaux.** Recommandé (retenu ci-dessus) : v1 reçoit les `LOG` comme avant ; v2 doit faire `SUBSCRIBE logs`. Les journaux restent ouverts ; à revoir si un secret pouvait un jour y apparaître.
- **D7 — Mot de passe au lieu d'une clé aléatoire.** Recommandé : clé aléatoire 256 bits (§9.6). Un mode « phrase secrète » (PBKDF2-HMAC-SHA256 calculé sur le PC) est possible plus tard ; il exige `openssl` ou `python3` côté PC.
- **D8 — Écoute en boucle locale seule** (drapeau dans CONFIG) pour ne passer que par SSH : utile, hors V1.1.
- **D9 — Application à chaud du nom DHCP** (relancer `udhcpc`) : refusée tant qu'on ne sait pas éviter `deconfig` ; reboot demandé par l'utilisateur.
- **D10 — Parsing strict en v1.** Recommandé : ne pas changer (`atoi`) pour ne pas casser d'anciens scripts ; v2 strict.
- **D11 — Source de `fw.version`.** `git describe` (déjà dans l'image) ; un fichier `VERSION` de release (par `make_kit.sh`) serait plus lisible ; à décider avec l'agent release.

---

## Décisions utilisateur (2026-11-09) — font foi

- **D1** : auth des écritures **désactivée par défaut**, proposée par le wizard (défaut « non »). Lectures toujours ouvertes sur le LAN.
- **D2** : hostname par défaut **`akam-XXXX`** (4 derniers chiffres hex de la MAC wlan0), modifiable ; remplace `IPCamera`.
- **D7** : secret = **clé aléatoire 256 bits** générée par le wizard (stockée côté PC, jamais un mot de passe humain).
- Autres décisions (D3–D6, D8–D11) : recommandations de la spec retenues.
- Calendrier : **V1.1** (après la release v1.0).
