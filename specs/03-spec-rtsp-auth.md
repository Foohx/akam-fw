# Akam — Spécification : authentification RTSP Digest (optionnelle)

Statut : rédigée le 2026-11-09 (agent DEV, branche `feature/rtsp-digest-auth`). Seule cette spec est en français ; code, messages, README et docs sont en anglais.

## 0. Décisions actées (utilisateur)

- Authentification RTSP **Digest** (RFC 2617 ; compatibilité MD5 de la RFC 7616) pour VLC/live555, ffmpeg, go2rtc, Frigate.
- **Désactivée par défaut.** Le wizard la **propose** (opt-in, défaut « non »).
- Gérable sans reflasher : commande caméra via SSH et via le wizard.
- JAMAIS gérable par le port 8091 (non authentifié) ni par un autre port ouvert.

## 1. Fichier de configuration

`/etc/config/akam/rtsp_auth` (partition CONFIG, jffs2, persistante, `0600`, répertoire `0700`). Une seule ligne, format `htdigest` :

```
user:akam:<HA1>      HA1 = md5hex("user:akam:password"), 32 hexa minuscules
```

- Le mot de passe en clair n'est jamais stocké, ni transmis à la caméra, ni journalisé (le wizard n'envoie que la ligne ci-dessus).
- `user` : 1 à 32 caractères parmi `A-Z a-z 0-9 . _ @ + -` (pas de `:`, ni d'espace, ni de caractère à échapper dans une URL). Realm fixe : `akam`.
- Le HA1 dépend du realm : un fichier `user:autre:...` est invalide.

### États (le fichier est relu à chaque requête protégée et chaque seconde pendant un flux : 70 octets, pas de redémarrage)

| Fichier | État | Comportement |
|---|---|---|
| absent (`ENOENT`) | OFF | aucune authentification (comportement actuel) |
| présent et valide | ON | Digest exigé |
| présent mais illisible/invalide | BROKEN | **fermé** : toute requête protégée est refusée (401 sans possibilité de réussir) ; journalisé. Un fichier que l'utilisateur voulait protéger ne doit pas devenir « ouvert » en silence ; `akam_rtsp_auth.sh disable` ou `set` répare. |

Le fichier est écrit de façon atomique (tmp + `mv`) : un lecteur voit l'ancien ou le nouveau contenu, jamais un fichier tronqué.
Limite connue : la CONFIG est montée avant le lancement de `ak_rtsp` (wrapper lancé par le watchdog) ; si elle ne l'était pas, le flux serait ouvert jusqu'à la prochaine requête après montage (rechargement à chaud).

## 2. Protocole (ak_rtsp)

- Méthodes protégées : tout sauf `OPTIONS` (DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, SET_PARAMETER, inconnues). `OPTIONS` reste ouvert : keepalive de ffmpeg/live555 et sonde de disponibilité.
- Défi : `401 Unauthorized` + `WWW-Authenticate: Digest realm="akam", nonce="<32 hexa>", algorithm=MD5`, plus `stale=true` quand les identifiants sont corrects mais le nonce périmé/inconnu.
- **Pas de `qop`** dans le défi : live555 (VLC) et go2rtc n'implémentent que la forme RFC 2069 (`response = MD5(HA1:nonce:MD5(méthode:uri))`) ; ffmpeg gère les deux. Si un client répond quand même avec `qop=auth`, `nc` et `cnonce`, la forme RFC 2617 est vérifiée aussi (`nc` strictement croissant).
- Algorithme : absent ou `MD5` (insensible à la casse). `MD5-sess`, `SHA-256`... : refus.
- Nonce : 16 octets de `getrandom()` (repli `/dev/urandom` ; échec = refus fermé), **par connexion**, durée de vie 120 s pour s'authentifier. Un en-tête rejoué sur une autre connexion échoue (nonce inconnu).
- Une connexion authentifiée le reste tant que la configuration ne change pas : les requêtes suivantes sans en-tête `Authorization` sont acceptées (certains clients ne le renvoient pas sur TEARDOWN/GET_PARAMETER) ; une requête AVEC en-tête doit vérifier. Pas de limite d'âge du nonce après authentification (keepalives longs).
- Changement de configuration (activation, changement d'identifiants, désactivation) : le flux en cours est coupé dans la seconde si la connexion n'est plus valide (activation ou nouveaux identifiants ; la désactivation ne coupe rien). Le client se reconnecte et reçoit le défi.
- Comparaison de l'utilisateur et de la réponse en **temps constant** (le hash est calculé même si l'utilisateur est faux : pas de distinction « utilisateur inconnu »).
- `uri` de l'en-tête : haché tel qu'envoyé (un seul média sur le serveur), pas de comparaison avec la ligne de requête (chemins divergents selon les clients).
- Taille bornée de chaque paramètre d'en-tête (refus, pas de troncature).

## 3. Échecs, limitation de débit, journaux

- Un échec = en-tête mal formé ou réponse fausse (pas : absence d'en-tête, ni `stale`). Compteur global au processus, remis à zéro après 60 s sans échec. Délai avant la réponse 401 : échecs 1-2 : 0 ; 3 : 500 ms ; 4 : 1 s ; ≥ 5 : 2 s. 5 échecs sur une même connexion : connexion fermée. (Le serveur ne sert qu'un client à la fois : le délai est le prix d'un essai, il ne peut pas bloquer plus que la connexion fautive.)
- Limites connues (déni de service) : le serveur n'a qu'une place. Le délai ne coûte jamais plus de 0 + 0 + 0,5 + 1 + 2 s (3,5 s) à une connexion fautive avant sa fermeture, et seul un échec (jamais une requête sans en-tête ni un client authentifié) est ralenti. Mais un client du réseau local qui se connecte et se tait occupe la place sans s'authentifier (il n'y a pas de délai d'inactivité avant PLAY, avec ou sans mot de passe : comportement antérieur) ; seul le contrôle d'accès réseau (VLAN, Wi-Fi invité) l'empêche. L'authentification protège le flux, pas la disponibilité.
- Limites connues (protocole) : Digest sans `qop` ne protège pas d'un attaquant actif sur le chemin (il peut rejouer un défi) ni de l'écoute du flux ; le HA1 du fichier est équivalent à un mot de passe pour Digest (fichier 0600, jamais exposé par 8091) ; `uri` n'est pas comparé à la ligne de requête (voir plus haut), ce qui est sans effet puisque le nonce est propre à la connexion.
- Journal `[ak_rtsp] auth: ...` : motif, compteur, adresse du client. JAMAIS le nom d'utilisateur saisi, l'en-tête, la réponse, le mot de passe, le HA1. Le journal de requête masque aussi un éventuel `user:pass@` dans l'URI (`rtsp://***@hôte/`).

## 4. Commande caméra `/usr/sbin/akam_rtsp_auth.sh` (POSIX sh, BusyBox 1.30.1, sans `tr` ni `mktemp`)

- `set USER` : mot de passe lu deux fois sur le terminal sans écho (`stty -echo`, restauré par `trap`), longueur ≥ 8 ; HA1 par `md5sum` BusyBox (le mot de passe passe par un pipe, pas par `argv`) ; écriture atomique 0600.
- `set-hash` : lit UNE ligne `user:akam:HA1` sur stdin (utilisée par le wizard : le mot de passe est tapé et haché sur le PC), la valide, l'installe. Aucun mot de passe n'est jamais dans `argv`.
- `show` : utilisateur seulement. `disable` : supprime le fichier. `status` : `enabled (user X)`, `disabled`, ou `INVALID` (code retour 3 pour INVALID).
- Installée dans l'image (`tools/release/image_files.txt`).

## 5. Wizard

- `./akam rtsp-auth [set|disable|status]` (SSH avec la clé de l'utilisateur) : mot de passe saisi localement sans écho, deux fois, ≥ 8 caractères ; haché localement ; envoyé comme ligne `user:akam:HA1` par stdin de ssh (`akam_rtsp_auth.sh set-hash`).
- Étape `verify` : après les vérifications réussies, question « Protect the video stream with a username/password? [y/N] » (défaut non ; sans terminal : non ; `--dry-run` : montrée, rien d'envoyé).
- `akam_hwcheck.sh` : ligne `RTSP auth: enabled (user X)` / `disabled` (information, jamais un échec).

## 6. Tests

MD5 (RFC 1321), Digest (exemple RFC 2617 §3.5 et RFC 2069), machine d'état hôte (401 → bonne réponse → 200 ; mauvais mot de passe ; nonce périmé ; rechargement du fichier ; BROKEN ; limitation), script caméra sous BusyBox (pinné), wizard en `--dry-run` et contre la fausse caméra, sentinelle de secret dans les journaux, contrôle croisé avec `ffprobe` (client Digest réel) quand ffmpeg est installé.
