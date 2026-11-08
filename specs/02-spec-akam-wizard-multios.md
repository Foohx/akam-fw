# Akam — Spécification : assistant guidé (wizard), hôtes multi-OS, installation sans compilation

Statut : rédigée le 2026-11-08 par l'agent SPEC. Base : `specs/00-brief-akam-v1.md` (fait foi côté produit) et `specs/01-spec-akam-v1.md` (règles dev §0 héritées : code/commentaires/logs/docs en ANGLAIS, une tâche = une branche `feature/...` issue de `dev`, jamais de push/tag/main, `shellcheck` propre, tests `tests/*_test.sh`).
Cette spec ne contient aucun code d'implémentation. Seule cette spec est en français ; tout ce que les tâches produisent (wizard, messages, README, docs) est en anglais.

Objectif utilisateur : « tout le processus doit être super simple ».
1. Un outil côté PC qui guide pas à pas et fait presque tout.
2. Un README limpide pour un débutant.
3. Hôtes supportés : Debian/Ubuntu et macOS, pas seulement Arch.
4. Installation possible SANS compiler (kit de release précompilé + dump APP de l'utilisateur).

## 0. Décisions actées (par l'utilisateur)

- **DA1.** L'approche « kit précompilé / installation sans compilation » (§4) est APPROUVÉE et devient le chemin par défaut du wizard ; « compiler depuis les sources » reste une option.
- **DA2.** La langue de tout ce que voit l'utilisateur (invites, messages, erreurs, README, docs) est l'ANGLAIS. Pas de localisation en v1 (pas d'option `--lang`).

Rien de ce qui suit n'a été exécuté sur une caméra par l'agent SPEC. Le chemin « réel » du §1 est celui que l'utilisateur a suivi à la main.

---

## 1. Le chemin réel à automatiser (référence)

| # | Étape manuelle vécue | Où | Automatisable ? |
|---|---|---|---|
| 1 | Vérifier les outils de l'hôte, installer les dépendances | PC | Oui (détection OS + commande d'installation affichée/exécutée avec accord) |
| 2 | Obtenir le kit (ou compiler depuis les sources) | PC | Oui |
| 3 | Identifier le périphérique microSD | PC | Oui (liste filtrée amovible/USB, confirmation modèle+taille) |
| 4 | Préparer la microSD toolkit pinné (telnet:24, OFFLINE=1, wifi.conf, hack.sh, dump.done) | PC | Oui (`prepare_first_boot_sd.sh`, étendu macOS) |
| 5 | Insérer la carte, démarrer la caméra, trouver son IP | humain + PC | Partiel (scan/ping, vendor « AltoBeam » ; sinon saisie) |
| 6 | Shell root telnet:24 : double `dd` des 8 partitions + sha256 | caméra | Oui via session telnet scriptée (§2.4) |
| 7 | Copie des dumps vers le PC + vérif sha256 | caméra→PC | Oui (nc + `/dev/tcp`, §2.5) |
| 8 | Construire l'image Akam depuis le dump APP de l'utilisateur | PC | Oui (chemin sans compilation, §4) |
| 9 | MicroSD de premier boot (toolkit + AKAM_MAINTENANCE + `akam/authorized_keys` + fichiers de flash, aucun `*.bin` à la racine) | PC | Oui |
| 10 | Telnet : `mount --bind /tmp/sd /mnt` si besoin, `install_with_flash_tool.sh` (YES tapé), relecture md5 | caméra | Oui (YES tapé par l'humain, jamais auto) |
| 11 | Coupure/remise sous tension avec carte, SSH par clé + `akam_hwcheck.sh` | caméra | Oui, tout par SSH |
| 12 | Retirer la carte, redémarrer, vérifier ports 22/554/8091 seulement | humain + PC | Oui (scan de ports côté PC) |
| 13 | Mises à jour sans carte : `reflash_over_ssh.sh` (jamais testé matériel) | PC→SSH | Oui, marqué « untested on hardware » |

Difficultés réellement rencontrées, à traiter explicitement : identifier le périphérique SD ; VLC (`vlc-plugin-live555`, `vlc-plugin-ffmpeg`, `--avcodec-hw=none`) ; trouver l'IP ; collage multi-lignes qui consomme le `YES` (une commande à la fois) ; pare-feu PC bloquant l'entrant (le PC se connecte toujours VERS la caméra).

---

## 2. A — Le wizard

### 2.1 Nom et point d'entrée

`./akam` à la racine du dépôt (et du bundle de release). Sous-commandes : `./akam` (assistant complet, reprend où il s'est arrêté) ; `status` ; `step <nom>` (rejoue une étape, idempotente) ; `doctor` (vérifie l'hôte, ne modifie rien) ; `update` (reflash par SSH, sans carte) ; `restore`. Options globales : `--dry-run`, `--yes-safe` (accepte seulement les confirmations non destructives, JAMAIS le flash ni l'effacement), `--state-dir DIR`, `--verbose`, `--no-color`.
Les scripts `tools/...` restent utilisables seuls ; le wizard est une couche d'orchestration. La logique va dans `tools/wizard/lib/*.sh` (testable) ; `./akam` ne fait que dispatcher.

### 2.2 Langage : comparaison et recommandation

| Critère | bash 3.2-compatible | POSIX sh | Python 3 stdlib |
|---|---|---|---|
| Présent par défaut macOS | Oui (`/bin/bash` 3.2) | Oui | NON : `python3` n'est qu'un shim qui déclenche l'installation des Command Line Tools |
| Présent par défaut Debian/Ubuntu | Oui | Oui (dash) | Souvent, pas toujours (Debian minimale) |
| Cohérence avec le dépôt | Totale (shellcheck, tests existants) | Bonne, perd tableaux/`[[ ]]`/`local` | Nouvelle chaîne d'outils |
| UI interactive | Correcte | Pauvre | Meilleure |
| Sockets (telnet, transfert) | `/dev/tcp` natif, OK en 3.2 macOS | Absent | Excellent (`telnetlib` supprimé en 3.13) |

**Recommandation (décision D1 ci-dessous) : bash, compatible bash 3.2, `#!/usr/bin/env bash`.** Contraintes imposées par lint (§6.3) : pas de `declare -A`, `mapfile`/`readarray`, `${v,,}`/`${v^^}`, `&>>`, `|&`, `[[ -v ]]`, `coproc`, `read -t` fractionnaire. Ni Python ni `expect`.

### 2.3 UX

- Étapes numérotées « Step N/9 — titre ». Pour chacune : **What**, **Why**, **Risk** (none / writes to the SD card / writes to the camera flash), **Command** exacte affichée avant exécution, puis `[Enter] continue / s skip / q quit`.
- Noms stables : `doctor`, `get-kit` (ou `build`), `prepare-backup-sd`, `find-camera`, `backup`, `image`, `prepare-flash-sd`, `flash`, `verify`, `finish`.
- **État reprenable** : fichier `state` (`clé=valeur`, sourçable, jamais de secret) dans `${XDG_STATE_HOME:-$HOME/.local/state}/akam/` (Linux) ou `$HOME/Library/Application Support/Akam/` (macOS) ; surcharge `--state-dir`/`AKAM_STATE_DIR`. Contient : version, étapes faites + horodatage, IP caméra, dossier de sauvegarde, sha256 des fichiers clés. Chaque étape vérifie l'état ET le disque (image disparue → étape redevient « à faire »).
- **Idempotence** : étape faite → « already done, redo? » (défaut non).
- **Confirmations destructives** : effacement de carte = saisie de `ERASE <disque>` ; flash = `YES` (déjà exigé par `install_with_flash_tool.sh`). `--yes-safe` et `--dry-run` ne contournent jamais. Sans TTY, refus.
- **Journal** `akam.log` (dans le dossier d'état) : chaque commande avant exécution, secrets masqués (§2.7).
- **Dry-run** : parcourt le scénario, vérifie l'hôte/kit/hash, affiche « WOULD run: ... » pour le reste. C'est le test d'intégration principal (sans caméra ni carte).
- Erreurs : une phrase + « what to do ». Couleurs seulement si TTY et `NO_COLOR` absent.

### 2.4 Automatiser la session telnet ? Évaluation

Contexte : shell root BusyBox sur telnet:24 (toolkit). Ni `expect`, ni SSH à ce stade.

| Option | Pour | Contre |
|---|---|---|
| (a) Afficher les commandes à coller | Zéro désynchronisation | Erreurs humaines réelles (collage multi-ligne avalant `YES`, mauvais device) : c'est la partie pénible |
| (b) Session scriptée en bash pur sur `/dev/tcp`, marqueurs de fin de commande | Aucune dépendance, portable, sorties capturables/vérifiables, `YES` tapé par l'humain | Négociation IAC à filtrer, écho/prompt à gérer, plus de code à tester |
| (c) `expect`/`telnet` externes | Moins de code | `telnet` absent par défaut (macOS récent, Debian) ; `expect` absent Debian |

**Recommandation (D4) : (b) par défaut, (a) en repli automatique** (`--manual-telnet`, ou après 2 échecs : une commande à la fois, « paste then come back »).
Conception (`tools/wizard/lib/camsh.sh`) : `exec 3<>/dev/tcp/$IP/24` ; chaque commande envoyée `cmd; echo "__AKAM_DONE_$N:$?__"` ; lecture bornée par timeout jusqu'au marqueur, code retour extrait ; octets IAC (`0xFF xx xx`) et `\r` filtrés ; une commande par envoi, < 400 octets ; ouverture sans effet sur la caméra. Le flash : le wizard lance `install_with_flash_tool.sh`, relaie l'invite, l'humain tape `YES` dans le wizard qui le relaie ; jamais envoyé par le code. Tests : fausse caméra locale rejouant des transcriptions.
SSH (étapes 11-13, `update`) : entièrement automatique avec la clé de l'utilisateur (`ssh -i`, `StrictHostKeyChecking=accept-new` à la première connexion seulement, empreinte affichée et mémorisée).

### 2.5 Transferts PC ↔ caméra

- PC→caméra : caméra `nc -l -p 8765 > fichier` (via camsh) ; PC `cat fichier > /dev/tcp/$IP/8765` (connexion sortante). Puis taille + hash comparés ; 3 essais max.
- Caméra→PC (dumps) : caméra `nc -l -p 8766 < dump.bin` ; PC `cat < /dev/tcp/$IP/8766 > dump.bin`. Les doubles `dd` + sha256 côté caméra restent faits d'abord.
- Si le `nc` de la caméra n'a pas `-l` : repli par la carte SD (déjà documenté).
- Aucun `nc` requis côté PC (bash 3.2 macOS supporte `/dev/tcp`).

### 2.6 Portes de vérification bloquantes

1. Kit : sha256 de chaque fichier ; signature si présente (§4.4) ; `kit_format` compatible.
2. Sauvegarde : 8 partitions, tailles attendues, copie 1 = copie 2 = copie PC, `SHA256SUMS` écrit.
3. Dump APP lisible (`unsquashfs -s`), arborescence `bin sbin local`.
4. Image : exactement 5242880 octets, aller-retour OK, aucune clé privée, sha256 enregistré ; `flash_guard.sh` et `preflight_check.sh` passent.
5. Carte de flash : fichiers présents, aucun `*.bin` à la racine, `authorized_keys` = clé publique valide (refus si en-tête `PRIVATE KEY`).
6. Flash : relecture md5 de la partition = md5 de l'image avant d'inviter au redémarrage.
7. Après boot : SSH par clé OK, `akam_hwcheck.sh` sans échec bloquant, ports ouverts = {22, 554, 8091} après retrait de la carte.

### 2.7 Sauvegardes et secrets

- Dossier de sauvegarde par défaut `~/akam-backup/<date>-<ip>/`, hors dépôt, jamais committé (`.gitignore` : `*.bin`, `*.img`, `akam-backup/`) : 8 partitions, `SHA256SUMS`, `README.txt`.
- **Double copie, confirmation bloquante** : « Copy this folder to a second medium BEFORE continuing: it is the only way back to the original firmware. » Le wizard peut copier (`--backup-copy DIR`) et revérifier les hashs.
- Dumps et images dérivées JAMAIS publiés (brief) : rappelé.
- **Secrets** : mot de passe Wi-Fi lu sans écho (`read -s`), passé par variable d'environnement (`AKAM_SD_PASS`) et non en argument (visible dans `ps`), jamais dans `state` ni `akam.log`, `set +x` forcé, masquage `***`. Rappel : il est en clair sur la carte → carte secrète, effacer `wifi.conf` en fin de procédure.
- Clé SSH : proposition de clé ed25519 dédiée (`~/.ssh/akam`) ; seule la `.pub` va sur la carte ; la privée n'est ni lue ni copiée.

### 2.8 Récupération

- Avant le flash rien d'irréversible (cf. `docs/akam_v1_backup_sd.md` §10).
- Après flash sans boot/SSH : arbre de décision (attendre, vérifier carte/IP, retirer la carte, UART) puis `./akam restore` selon `docs/akam_v1_flash_and_restore.md`.
- SSH jamais validé → le wizard ne dit pas « done » et ne propose pas de retirer la carte (le telnet de maintenance est le filet).
- Risque coupure de courant pendant le flash : affiché avant le YES.

---

## 3. B — Hôtes multi-OS (Debian/Ubuntu, macOS Intel + Apple Silicon)

### 3.1 Support et statuts

| Hôte | Statut v1 proposé | Testé ici ? |
|---|---|---|
| Arch Linux x86_64 | Supporté | Oui |
| Debian 12+/Ubuntu 22.04+ x86_64 | Supporté ; test conteneur/VM si dispo, sinon « untested » | Partiel |
| Debian/Ubuntu aarch64 | Prévu, non testé | Non |
| macOS 13+ Intel et Apple Silicon | **« untested on macOS »** jusqu'au retour d'un testeur | Non |
| Windows | Hors périmètre (WSL2 = Debian/Ubuntu) | — |

### 3.2 Dépendances

Sans compilation (défaut) : `curl`, `tar`, `xz`, hash (helper), `squashfs-tools`, `ssh`/`ssh-keygen`, outils carte SD. **Ni zig, ni cmake, ni make, ni gdb.**
Depuis les sources : en plus `make git bzip2 file shellcheck`, Zig pinné, désassembleur ARM.

| Besoin | Arch | Debian/Ubuntu | macOS (Homebrew) |
|---|---|---|---|
| squashfs | squashfs-tools | squashfs-tools (≥ 4.4, version vérifiée) | `squashfs` (mksquashfs/unsquashfs) |
| xz / bzip2 | xz bzip2 | xz-utils bzip2 | `xz` |
| curl, git, tar, file | idem | curl git tar file | système (git via CLT) |
| Carte SD | dosfstools util-linux parted | dosfstools util-linux parted | rien (diskutil) |
| shellcheck (dev) | shellcheck | shellcheck | `shellcheck` |
| coreutils GNU | natif | natif | `coreutils` optionnel, JAMAIS exigé |
| readelf/désassembleur ARM | binutils gdb | binutils gdb (gdb-multiarch) | `llvm` (llvm-readelf, llvm-objdump) |
| busybox (tests ash) | binaire pinné x86_64 | idem | §3.7 |
| qemu | optionnel | optionnel | optionnel |

`check_host.sh` : détecte l'OS (`uname -s`) et le gestionnaire (pacman/apt/brew), imprime la commande adaptée, ne lance jamais sudo seul (`./akam doctor` peut proposer de l'exécuter avec accord). Homebrew absent : afficher l'instruction officielle, ne pas l'installer.

### 3.3 Zig pinné par OS/arch

`deps.lock` : clés `ZIG_URL_<os>_<arch>` / `ZIG_SHA256_<os>_<arch>` pour `linux_x86_64` (existant, `24aeeec8...716c`), `linux_aarch64`, `macos_aarch64`, `macos_x86_64`. Tarballs 0.14.1 : `zig-<arch>-<os>-0.14.1.tar.xz`. Les sha256 DOIVENT être recopiés de `https://ziglang.org/download/index.json` (champ `shasum`) et recoupés avec la signature minisign ; aucun hash inventé. `bootstrap.sh` choisit via `uname -s`/`uname -m` (`arm64`→`aarch64`) ; répertoire d'extraction plus codé en dur `x86_64-linux` ; échec clair sur plateforme inconnue. La reproductibilité inter-hôtes n'est PAS garantie : seul l'hôte de référence du mainteneur produit le kit.

### 3.4 Carte microSD

`prepare_first_boot_sd.sh` (Linux : `lsblk wipefs sfdisk mkfs.vfat partprobe blockdev`) est scindé : partie commune (arbre de la carte, toolkit pinné, édition portable) + backends `tools/sd/backend_linux.sh` / `backend_macos.sh`.

| Opération | Linux | macOS |
|---|---|---|
| Lister | `lsblk -dnro NAME,TYPE,RM,TRAN,SIZE,MODEL` | `diskutil list external physical` + `diskutil info -plist /dev/diskN` (`RemovableMedia`, `BusProtocol`, `TotalSize`) |
| Garde-fous | existants (disque entier, amovible/USB) | `/dev/diskN` entier seulement (refuser `diskNsM`), jamais interne/système/synthétisé, taille plausible |
| Démonter | `umount` | `diskutil unmountDisk /dev/diskN` |
| Partition + FAT32 MBR | `wipefs`, `sfdisk`, `mkfs.vfat -F 32 -n AKAMSD` | `diskutil eraseDisk FAT32 AKAMSD MBRFormat /dev/diskN` (label ≤ 11 maj.) |
| Montage | mount/udisks | `/Volumes/AKAMSD` |
| Éjection | `sync` + eject | `diskutil eject /dev/diskN` |
| Privilèges | sudo | sudo ; macOS peut demander l'accès « volumes amovibles » au terminal → message |

Périphérique toujours confirmé (modèle, taille, nom) puis `ERASE diskN`. Tests : `--dry-run --stage-dir` existant + stubs `lsblk`/`diskutil` avec fixtures.

### 3.5 Différences GNU/BSD et bibliothèque portable

Un fichier `tools/lib/portable.sh` (fonctions `pt_*`) sourcé par les scripts exécutés côté HÔTE. Les scripts exécutés SUR LA CAMÉRA (BusyBox : `akam_hwcheck.sh`, partie distante de `akam_reflash_ssh.sh`, `akam_ssh.sh`, `akam_lockdown.sh`, `start_rtsp.sh`) ne dépendent pas du helper.

| Sujet | GNU (actuel) | macOS/BSD | Remède |
|---|---|---|---|
| sha256 | `sha256sum` | `shasum -a 256` | `pt_sha256 FILE` |
| md5 | `md5sum` | `md5 -q` | `pt_md5 FILE` |
| taille | `stat -c%s` | `stat -f%z` | `pt_size` (`wc -c <`) |
| édition en place | `sed -i` | `sed -i ''` | `pt_sed_i` (temp + `mv`) |
| chemin absolu | `readlink -f` | absent < 12.3 | `pt_realpath` (`cd`+`pwd -P`) |
| date | `date -d @N` | `date -r N` | `pt_date_from_epoch` |
| timeout | `timeout` | absent | `pt_timeout` (timeout/gtimeout/sous-shell) |
| mktemp | gabarit libre | gabarit requis | `mktemp -d "${TMPDIR:-/tmp}/akam.XXXXXX"` |
| dd | `bs=1M`, `status=` | `bs=1m`, pas de `status=` | pas de `status=` côté hôte |
| options GNU (`xargs -r`, `grep -P`, `find -printf`, `sort -V`, `ls --`) | oui | non | interdites côté hôte (lint) |
| tar reproductible | `--sort --mtime --owner` | bsdtar différent | kit fabriqué sur Linux seulement ; extraction `tar -xf` portable |
| nproc | oui | `sysctl -n hw.ncpu` | `pt_ncpu` |
| nc | variantes | BSD | `/dev/tcp` côté hôte |
| readelf | binutils | absent | `pt_readelf` (llvm-readelf) |
| Zig dir | `x86_64-linux` | autre | variables `deps.lock` |

Scripts hôte à modifier : `tools/build/{bootstrap,check_host,env,check_time32}.sh`, `tools/dropbear/build_dropbear.sh` (`stat -c`), `src/ak_rtsp/build_firmware.sh` (`sha256sum`, `date`), `src/ak_rtsp/Makefile` (chemin Zig), `tools/sd/prepare_first_boot_sd.sh` (`sed -i`, `lsblk`…), `tools/flash_scripts/reflash_over_ssh.sh` et partie hôte de `akam_reflash_ssh.sh` (`stat -c%s`), `flash_guard.sh` et `preflight_check.sh` (à relire), `tests/*_test.sh` (`sha256sum`, `sed -i`, BusyBox x86_64). L'inventaire complet est le premier livrable de T1.

### 3.6 `check_time32.sh` sans gdb

Le script désassemble `.text` avec gdb. gdb est pénible sur macOS. Remède : `pt_disasm ELF` essaie `arm-none-eabi-objdump`/`objdump` (binutils ARM), `llvm-objdump` (options à valider), puis gdb. `tests/time32_shim_test.sh` doit donner des verdicts identiques avec chaque outil disponible. **Dans le chemin kit, la porte time64 est franchie chez le mainteneur (§4.2) : l'utilisateur n'a besoin d'aucun désassembleur.**

### 3.7 Tests (busybox, qemu)

BusyBox pinné = x86_64 Linux, inexécutable sur macOS. Sur macOS : `busybox` Homebrew si présent, sinon `SKIP (needs busybox ash)`, sortie 0 avec compteur de SKIP visible. Aucun qemu requis.

### 3.8 Pièges macOS pour l'image

1. **FS insensible à la casse (APFS par défaut)** : un arbre squashfs peut contenir des noms ne différant que par la casse → collisions silencieuses. Détection (créer `a`/`A`) ; si insensible : travail dans une image disque sensible à la casse (`hdiutil create -type SPARSE -fs "Case-sensitive APFS"`) ou refus expliqué.
2. **Propriétaires/nœuds spéciaux** : `unsquashfs` non-root perd uid/gid et ne crée pas de nœuds. Contrôle `unsquashfs -ll` : tout uid/gid 0 et aucun nœud → `mksquashfs -all-root` ; sinon root/`fakeroot` sous Linux, refus expliqué sous macOS. À CONFIRMER sur le vrai dump APP.
3. **Versions squashfs-tools** : sortie à comparer (Arch vs Debian) avant d'affirmer l'identité binaire ; sinon documenter « contenu identique, octets potentiellement différents » (aller-retour + liste de fichiers = preuve).
4. FAT32 sans permissions : aucun bit exécutable attendu depuis la carte (scripts lancés par `sh`).

---

## 4. Installation SANS compilation : le kit de release (approuvé, DA1)

### 4.1 Principe et frontière légale

On ne publie JAMAIS d'image APP complète (elle embarque l'APP propriétaire : binaires Anyka/Tuya, `.ko`, scripts) ni de dump. On publie des composants Akam (nos binaires, scripts, licences) ; l'utilisateur fournit SON dump APP ; l'assemblage est local.
```
dump APP de l'utilisateur --unsquashfs--> arbre
  + overlay du kit (ak_rtsp, wrapper, ini, dropbearmulti, akam_ssh.sh, akam_lockdown.sh, licences, MANIFEST)
  + patch de sbin/service.sh (telnet/ftp off)
  --mksquashfs xz--> pad 5242880 --> vérifs --> SHA-256
```
Côté utilisateur : `squashfs-tools` seulement. Ni zig, cmake, make, compilateur, gdb.

### 4.2 Contenu et fabrication

Asset de release `akam-kit-<version>.tar.xz` + `SHA256SUMS` + `SHA256SUMS.sig`. Arborescence :
```
akam-kit-<version>/
  MANIFEST.txt
  bin/ak_rtsp  bin/dropbearmulti  bin/flash_tool     # ARM EABI5 soft-float, statiques, strippés
  overlay/        # wrapper ak_rtsp, _ht_process.ini, akam_ssh.sh, akam_lockdown.sh, liens dropbear
  patches/service_sh.sh
  licenses/       # LICENSE (MIT), THIRD_PARTY_NOTICES.md, LICENSE.dropbear, musl COPYRIGHT, Zig LICENSE (compiler-rt), licences SmolRTSP + 4 dépendances header-only, licence amont ak_rtsp
  SOURCES.txt     # offre de sources : commit, URL+sha256 des archives (deps.lock), patches Dropbear
```
Également attaché : `akam-<version>-src.tar.gz` (`git archive` du tag : conformité licences + voie « compiler soi-même »).
`MANIFEST.txt` : `kit_format=1`, `akam_version`, `git_commit`, `built_utc` (SOURCE_DATE_EPOCH), `deps_lock_sha256`, `zig_version`, `target=arm-linux-musleabi`, `time64_gate=pass` (outil, version, date), sha256 de chaque fichier, et le statut de validation matérielle repris du README.
Fabrication (`tools/release/make_kit.sh`, hôte Linux de référence du mainteneur, SANS CI) : build → **porte time64 bloquante** sur `ak_rtsp` ET `dropbearmulti` → arborescence normalisée (`tar --sort=name --mtime=@$SOURCE_DATE_EPOCH --owner=0 --group=0 --numeric-owner`, `xz`) → `SHA256SUMS` → signature. `--verify-reproducible` reconstruit proprement et compare ; écart = blocage ou documenté. Publication par l'utilisateur : `gh release create`/`gh release upload` en local.

### 4.3 Récupération par le wizard

`get-kit` : (1) `--kit-file` local ; (2) téléchargement HTTPS strict (`curl --proto '=https'`) depuis l'URL de release dérivée de la version du wizard ; (3) repli « build from source » proposé. Version du kit = version du wizard, ou `kit_format` listé dans `tools/wizard/kit_compat.txt`, sinon refus expliqué.

### 4.4 Vérification

1. **sha256 obligatoire** ; le hash du kit est aussi dans les notes de release, `CHANGELOG.md` et le message du tag annoté (recoupement par un autre canal ; `--paranoid` demande de comparer les 12 premiers caractères).
2. **Signature recommandée** : `ssh-keygen -Y sign -n akam-release` sur `SHA256SUMS` ; vérification `ssh-keygen -Y verify -f tools/release/allowed_signers -I akam-release -n akam-release`. Aucune installation (ssh-keygen déjà requis, présent macOS/Debian), contrairement à minisign/gpg. Clé publique commitée ET citée dans le README. Limite honnête : n'a de valeur que si le wizard/dépôt obtenu est authentique. Signature présente → obligatoire ; absente → avertissement + confirmation tapée (`--allow-unsigned`).
3. Rien du kit n'est exécuté sur le PC (binaires ARM + scripts destinés à l'image).

### 4.5 `build_firmware.sh --kit DIR`

Deux sources d'entrées, une seule fonction d'assemblage :
- Mode sources (actuel) : `AK_RTSP_BIN`, `DROPBEAR_MULTI`, `SMOLRTSP_DIR`, `MUSL_COPYRIGHT`, `ZIG_LICENSE`, templates du dépôt.
- Mode kit : `--kit DIR` (ou `AKAM_KIT`) lit binaires, overlay, patch `service.sh`, licences depuis `DIR` après vérification de `MANIFEST.txt` (sha256 par fichier, `kit_format`, `akam_version`). `--kit` n'abaisse AUCUNE vérification (modes, pas de clé privée, taille avant padding, aller-retour, SHA-256).
- Porte time64 en mode kit : pas de gdb ; `MANIFEST.txt` doit porter `time64_gate=pass` et les sha256 des binaires doivent correspondre. `--recheck-time64` rejoue la porte si un désassembleur ARM existe. Les variables `AKAM_ALLOW_*` gardent leur sens (stubs seulement).
- Le `MANIFEST.txt` du kit est embarqué dans l'image (déjà prévu) pour retrouver ce qui est flashé.
- Sortie : dossier `akam-image-<version>-<date>/` hors dépôt (image, sha256, rapport).

### 4.6 Documentation

README : chemin kit = chemin par défaut. `docs/akam_v1_build.md` → « Build from source (optional) ». Nouveau `docs/akam_v1_kit.md` : contenu, vérification, reproduction (« rebuild the kit and compare sha256 »), frontière légale, mode `--kit`.

---

## 5. C — Structure du README (débutant, anglais)

1. Titre + une phrase + avertissement matériel (modèle `3215672.3`, étiquette `D26228` à confirmer).
2. **Status** honnête et encadré (validé matériel / non) ; macOS « untested ».
3. **What you need** : la caméra, une microSD vide, un PC Linux ou macOS sur le même Wi-Fi, 30-45 min, un second support pour la sauvegarde.
4. **Install in 5 steps** : obtenir le bundle/kit → `./akam` → schéma des 9 étapes ; « the wizard does X, you do Y » (insérer/retirer la carte, brancher/débrancher).
5. **Safety in 5 lines** : sauvegarde + double copie ; jamais d'image/dump publié ; flash = `YES` ; chemin de restauration ; Wi-Fi en clair sur la carte.
6. **After install** : RTSP, VLC (`vlc-plugin-live555`, `vlc-plugin-ffmpeg`, `--avcodec-hw=none`), port 8091, SSH.
7. **Troubleshooting** : trouver l'IP (liste DHCP, fabricant « AltoBeam »), identifier la carte, coller une commande à la fois, pare-feu, `./akam doctor`.
8. **Update** (`./akam update`, « untested on hardware » tant que vrai) et **Restore**.
9. **Advanced** : install manuelle (`docs/akam_v1_*.md`), build from source, structure du dépôt.
10. Fork/Credits/Licences en bas (contenu inchangé).
Règles : pas de jargon sans définition, une commande par bloc, matrice OS en tableau, aucune promesse « validé » non démontrée, `tests/readme_links_test.sh`.

---

## 6. D — Découpage en tâches

Règles communes : un agent par tâche, branche `feature/<nom>` depuis `dev`, fichiers exclusifs, anglais, `shellcheck -s bash` propre, tests ajoutés, ligne `CHANGELOG.md` (Unreleased ; conflits triviaux résolus par le coordinateur), rapport avec sortie de `tools/ci/local_check.sh`, jamais de push/tag.

### 6.1 Vagues (max 2 en parallèle, fichiers disjoints)

| Vague | A | B |
|---|---|---|
| 1 | T1 Bibliothèque portable + lints + contrôle local | T4 Fabrication/vérification du kit |
| 2 | T2 Dépendances hôte multi-OS | T3 Assemblage depuis le kit |
| 3 | T5 Carte SD multi-OS | T6 Noyau du wizard |
| 4 | T7 Étapes caméra | T8 README + docs |
| 5 | T9 Intégration, e2e dry-run, retrait de la baseline de lint | — |

### 6.2 `tools/ci/local_check.sh` (équivalent CI local, aucune minute de CI)

Ordre : `shellcheck -s bash` sur tous les `*.sh` et `akam` → lint portabilité → lint bash 3.2 → toutes les `tests/*_test.sh` (PASS/FAIL/SKIP) → `./akam doctor` + `./akam --dry-run` (dès T6) → aucun `*.bin`/dump/secret suivi par git. Sortie `LOCAL CHECK: OK (N tests, M skipped)`. Sur macOS, affiche `Platform: macOS (untested status)` et la liste des SKIP.

### 6.3 Garder macOS honnête sans macOS

1. **`tools/ci/lint_portable.sh`** : interdits côté hôte : `sed -i`, `stat -c`, `readlink -f`, `date -d`, `sha256sum`/`md5sum` hors `portable.sh`, `xargs -r`, `grep -P`, `find -printf`, `sort -V`, `declare -A`, `mapfile`, `readarray`, `${x,,}`, `${x^^}`, `|&`, `&>>`, `timeout` nu, `dd status=`. Exception `# portable-ok: <raison>` ; scripts caméra listés dans `tools/ci/camera_scripts.txt`.
2. **Shims BSD** : `tests/shims/bsdlike/` (PATH restreint) exposant `shasum`, `md5`, un `stat` rejetant `-c`, un `sed` rejetant `-i` sans argument, un `date` rejetant `-d` : prouve que les scripts passent par le helper. Exécution sous bash 3.2 si disponible.
3. **Fixtures réelles** : `tools/sd/collect_macos_fixtures.sh` (n'envoie rien nulle part) pour qu'un testeur fournisse `diskutil` plist/`uname`.
4. **Statut visible** : `./akam doctor` et README affichent « macOS: untested » tant que `tools/wizard/macos_verified.txt` (version, modèle de Mac, date, testeur) n'existe pas, créé uniquement sur retour réel.
5. **CI macOS (plus tard, hors v1)** : workflow `workflow_dispatch` uniquement (jamais sur push) lançant `local_check.sh` sur `macos-latest`, déclenché à la main (décision D9).

### 6.4 Tâches

**T1 — Portable lib + lint + local check** (`feature/portable-lib`)
Fichiers : `tools/lib/portable.sh`, `tools/ci/{local_check,lint_portable,lint_bash32}.sh`, `tools/ci/camera_scripts.txt`, `tests/shims/bsdlike/*`, `tests/portable_test.sh`, `tests/lint_test.sh`.
Contenu : fonctions `pt_*` (§3.5 + `pt_os`, `pt_arch`) ; lints ; contrôle local ; inventaire complet des usages à corriger dans le rapport (scripts existants non modifiés, sauf branchement du contrôle).
Acceptation : tests OK en GNU et sous shims BSD ; le lint détecte chaque motif interdit sur un fichier de test ; `local_check.sh` OK avec `tools/ci/lint_baseline.txt` listant les violations existantes (retirée par T2/T3/T5/T9).

**T4 — Fabrication et vérification du kit** (`feature/kit-maker`)
Fichiers : `tools/release/{make_kit,verify_kit,sign_kit}.sh`, `tools/release/allowed_signers` (clé fournie par l'utilisateur ; placeholder explicite sinon), `tools/release/README.md` (procédure `gh release upload`), `tests/kit_test.sh`, `docs/akam_v1_kit.md`.
Contenu : §4.2 et §4.4 ; `verify_kit.sh` côté utilisateur (sha256 + signature ssh + `kit_format`).
Acceptation : kit de fixture vérifié OK ; un octet modifié (fichier ou MANIFEST) → échec ; signature absente/invalide → comportement §4.4 ; deux constructions identiques (sha256) ; refus si la porte time64 échoue ; licences du kit = `THIRD_PARTY_NOTICES.md` (test) ; licence amont de `flash_tool` vérifiée et documentée (sinon inclusion bloquée, décision D7).

**T2 — Dépendances hôte multi-OS** (`feature/host-deps-multios`)
Fichiers : `tools/build/{deps.lock,bootstrap.sh,check_host.sh,env.sh,check_time32.sh}`, `tools/dropbear/build_dropbear.sh`, `src/ak_rtsp/Makefile` (chemin Zig seulement), `tests/bootstrap_test.sh`, `tests/time32_shim_test.sh`.
Contenu : §3.2, §3.3, §3.6, §3.7.
Acceptation : `check_host.sh` correct sur Arch (réel) et sur sorties simulées Debian/macOS (`AKAM_FAKE_OS`, tests seulement) ; bootstrap Linux x86_64 inchangé (même hash) ; autres plateformes testées par `--print-plan` sans téléchargement ; `check_time32` même verdict avec gdb et objdump sur binaires de référence ; baseline réduite.

**T3 — Assemblage depuis le kit** (`feature/build-from-kit`)
Fichiers : `src/ak_rtsp/build_firmware.sh`, `tests/build_firmware_test.sh`, `tests/build_from_kit_test.sh`, `tools/image/assemble_from_kit.sh` (enveloppe : décompression du dump APP, casse §3.8, `-all-root`/uid).
Acceptation : image en mode kit = même contenu (fichiers, modes, sha256) qu'en mode sources avec mêmes binaires ; refus si MANIFEST invalide, `time64_gate` ≠ pass, version/format incompatibles ; tests existants inchangés ; message clair sur FS insensible à la casse.

**T5 — Carte SD multi-OS** (`feature/sd-multios`)
Fichiers : `tools/sd/{prepare_first_boot_sd,backend_linux,backend_macos,collect_macos_fixtures}.sh`, `tests/first_boot_sd_test.sh`, `tests/sd_backend_test.sh`, `tests/fixtures/diskutil/*`.
Contenu : §3.4 ; `--list-candidates` lisible par machine ; saisie `ERASE <disque>` ; secret Wi-Fi par env/prompt seulement.
Acceptation : tests existants inchangés ; backend macOS (stubs `diskutil` plist) refuse disque interne, partition `diskNsM`, disque système, taille hors plafond ; commande finale exacte `diskutil eraseDisk FAT32 AKAMSD MBRFormat /dev/diskN` ; dry-run identique sur les deux OS.

**T6 — Noyau du wizard** (`feature/wizard-core`)
Fichiers : `akam`, `tools/wizard/lib/{ui,state,steps_host,log}.sh`, `tools/wizard/kit_compat.txt`, `tests/wizard_core_test.sh`.
Contenu : §2.1-2.3, 2.6 (portes 1, 3, 4, 5), 2.7, étapes `doctor`, `get-kit`/`build`, `prepare-backup-sd`, `image`, `prepare-flash-sd`, `status`, `--dry-run`, état reprenable. Tous les messages en anglais.
Acceptation : dry-run complet sans caméra ni carte ; reprise après interruption simulée ; état périmé détecté ; aucun secret dans `state`/`akam.log` (grep d'un mot de passe sentinelle) ; refus des actions destructives sans TTY ; lint bash 3.2 propre.

**T7 — Étapes caméra** (`feature/wizard-camera`)
Fichiers : `tools/wizard/lib/{camsh,transfer,steps_camera}.sh`, `tools/wizard/fake_camera.sh`, `tests/{camsh,transfer,wizard_camera}_test.sh`, `tests/fixtures/telnet_transcripts/*`.
Contenu : §2.4, §2.5 ; étapes `find-camera`, `backup` (double dd, sha256, copie PC, double copie), `flash` (YES relayé tapé), `verify` (SSH + `akam_hwcheck.sh` + ports), `update`, `restore`.
Acceptation : contre la fausse caméra : sauvegarde vérifiée, transfert corrompu détecté et réessayé, `YES` jamais envoyé sans saisie humaine (stdin vide → échec), repli manuel après 2 échecs, timeouts ; transcriptions tirées de `docs/akam_v1_backup_sd.md` (préciser ce qui est reconstitué, non enregistré).

**T8 — README + docs** (`docs/readme-newcomers`)
Fichiers : `README.md`, `docs/akam_v1_install.md` (guide manuel), `docs/akam_v1_build.md` (source optionnel), `docs/troubleshooting.md`, `tests/readme_links_test.sh`.
Acceptation : liens valides ; chaque `./akam ...` du README reconnu par `--help` ; aucune promesse hardware non démontrée ; cohérence avec noms d'étapes/options réels (lire `./akam --help` à la fin de la vague 3).

**T9 — Intégration** (`feature/wizard-integration`)
Fichiers : `tests/wizard_e2e_test.sh`, retrait de `lint_baseline.txt`, corrections transversales.
Acceptation : `local_check.sh` OK sans baseline ; scénarios dry-run « sans compilation » et « depuis sources » ; liste finale des SKIP/untested dans le rapport.

---

## 7. E — Décisions ouvertes pour l'utilisateur

**D1. Langage d'implémentation du wizard.** (a) bash 3.2-compatible ; (b) Python 3 stdlib ; (c) POSIX sh. Reco : (a) — seul présent sans installation partout (Python absent par défaut sur macOS), cohérent avec le dépôt.

**D2. Forme de distribution.** (a) `git clone` + kit téléchargé par le wizard ; (b) un bundle unique (wizard + scripts + kit) sans git. Reco : (b) pour les débutants, (a) pour les contributeurs (sinon macOS doit installer les CLT juste pour git). Coût : un asset de plus par release.

**D3. Support macOS pour v1.0.** (a) officiel ; (b) « best effort, untested » puis officiel après retour testeur ; (c) après v1.0. Reco : (b).

**D4. Profondeur d'automatisation telnet.** (a) commandes à coller ; (b) session scriptée `/dev/tcp` + repli manuel ; (c) (b) + flash lancé sans saisie. Reco : (b) ; le YES reste tapé par l'humain.

**D5. Wizard avant ou après le tag v1.0 ?** (a) v1.0 avec doc manuelle, wizard en v1.1 ; (b) retarder v1.0 jusqu'au wizard+kit ; (c) v1.0 = chemin manuel + kit (T4+T3), wizard ensuite. Reco : (c), une fois le chemin manuel validé en matériel (flash complet et boot Akam pas encore confirmés) ; ne pas annoncer « simple » avant validation matérielle.

**D6. Signature du kit.** (a) sha256 seul ; (b) sha256 + `ssh-keygen -Y` ; (c) + minisign/gpg. Reco : (b) ; l'utilisateur fournit une clé de signature dédiée (passphrase), publique commitée.

**D7. `flash_tool` précompilé dans le kit ?** (a) oui ; (b) non. Reco : (a) sous réserve que la licence amont autorise la redistribution binaire (T4 vérifie ; sinon l'utilisateur tranche).

**D8. `fakeroot`/`sudo` pour l'assemblage si le dump a d'autres uid que 0 ?** (a) refuser ; (b) autoriser sous Linux seulement. Reco : (b) Linux, (a) macOS ; décision finale après lecture des propriétaires réels du dump.

**D9. CI macOS manuelle (`workflow_dispatch`).** (a) jamais ; (b) plus tard, déclenchée à la main. Reco : (b) après v1.0.

---

## 8. Hypothèses et points non vérifiés

- Hashs Zig hors Linux x86_64 inconnus de l'agent SPEC : à relever par T2 sur ziglang.org.
- Comportement `unsquashfs`/`mksquashfs` non-root (uid, nœuds) sur le vrai dump APP non vérifié (§3.8, D8).
- `llvm-objdump` pour la porte time64 : options ARM à valider ; verdict identique à gdb exigé avant adoption.
- Reproductibilité bit-à-bit du kit et des images entre hôtes non démontrée ; seule celle du mainteneur est visée.
- `reflash_over_ssh.sh` / `akam_reflash_ssh.sh` jamais exécutés sur la caméra : `./akam update` doit l'afficher.
- Transcriptions telnet de test reconstituées depuis la doc, pas enregistrées.
