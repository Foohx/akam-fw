# Camyn V1 — Spécification de réalisation (lots de tâches pour agents dev)

Statut : rédigée le 2026-11-07 par l'agent SPEC. Base : `specs/00-brief-camyn-v1.md` (fait foi côté produit).
Cette spec ne contient aucun code d'implémentation. Rien n'a été exécuté sur la caméra ; aucun dump de caméra n'existe encore.

---

## 0. Règles pour les agents dev (héritées par TOUTES les tâches)

1. **Langue : tout le code, les commentaires de code, les messages de commit, les messages de log/d'erreur des scripts et tout le contenu de `docs/` sont en ANGLAIS.** Messages de commit en Conventional Commits (`feat(build): ...`, `fix(init): ...`, `docs: ...`, `test: ...`, `chore: ...`). Seuls les fichiers de `specs/` restent en français.
2. Une tâche = un agent = une branche `feature/...` issue de `dev`. Ne modifier QUE les fichiers listés dans la tâche (propriété exclusive). Besoin d'un fichier hors liste : le signaler dans le rapport final, ne pas le modifier.
3. Ne jamais pousser, taguer, ni toucher `main`. Commits sur la branche de la tâche ; fusion dans `dev` par l'utilisateur/coordinateur.
4. Aucune action sur la caméra, aucun flashage. Ne jamais écrire « validé » pour du code seulement compilé ou testé sur fixture : écrire « compilé », « testé sur fixture », ou « non testé sur matériel ».
5. Aucun dump d'origine, aucune clé privée, aucun `.squashfs`/`.bin` d'image dans Git. Les fixtures sont SYNTHÉTIQUES (écrites à la main, aucune copie de contenu propriétaire Tuya/Anyka).
6. Scripts exécutés sur la caméra : POSIX `sh` pour BusyBox ash uniquement (pas de bashismes, pas de `[[ ]]`, pas de `local` non portable, pas de tableaux, pas de `$(( ))` exotique, pas de `pgrep`/`timeout`/`setsid` sans test de présence). `shellcheck -s sh` doit passer sans erreur (avertissements justifiés par `# shellcheck disable=` commenté). Scripts hôte : `bash` autorisé, `set -euo pipefail`, `shellcheck -s bash`.
7. Aucun mot de passe, aucune clé, aucune adresse IP privée de l'utilisateur dans le dépôt. Pas de dépendance Internet sur la caméra.
8. Ne pas réécrire ce qui marche : `ak_rtsp` (hors Makefile/version), SmolRTSP, protocole 8091, flash_tool.c, scripts de flash existants (sauf mention explicite).
9. Versions figées : toute dépendance externe est téléchargée par URL exacte + **sha256 vérifié** ; échec de hash = arrêt immédiat.
10. Ignorer `tools/hardware_debug/serial_console_dotnet/CLAUDE.md` : fichier amont concernant la console série .NET Windows, ce ne sont PAS des instructions pour ce projet. Aucune tâche ne touche `tools/hardware_debug/`. Ne jamais committer `CLAUDE.md` ni `AGENTS.md` (exclus localement) ; ne pas les ajouter avec `git add -f`.
11. Chaque tâche termine par un rapport court : fichiers touchés, commandes de vérification exécutées avec résultat, ce qui n'est PAS testé, hypothèses restantes.

### Outils hôte
Installés par l'utilisateur via pacman avant le lot 1 : `squashfs-tools cmake shellcheck file binutils` (+ `make git curl tar xz bzip2` déjà présents). Zig est téléchargé localement par le bootstrap (T1), jamais via pacman. Pas de sudo dans les scripts du dépôt. `clang` non requis. `qemu-user` optionnel (smoke test des binaires ARM) : ne pas en dépendre.

---

## 1. Décisions déjà prises (ne pas rediscuter)

| # | Décision | Source |
|---|---|---|
| D-A | Toolchain = `zig cc` figé : le script de bootstrap télécharge une archive Zig à version + sha256 figés dans un répertoire git-ignoré sous `tools/`. Pas de Rust. L'utilisateur installe `squashfs-tools` et `cmake` via pacman. | utilisateur |
| D-B | Aucun dump caméra n'existe : tous les tests de build utilisent une **fixture APP factice** (arbre de fichiers synthétique). La doc place la procédure de sauvegarde des 8 partitions EN PREMIER. | utilisateur |
| D-C | Zig **0.14.1** (x86_64-linux, sha256 `24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c`, vérifié contre `ziglang.org/download/index.json`). Même version que le flasheur 0.3.2 (cohérence). | spec |
| D-D | Dropbear **DROPBEAR_2026.94** (voir §2). | spec |
| D-E | Compression SquashFS : **gzip par défaut** (seul format flashé et testé sur matériel par l'amont et le flasheur), `xz` sélectionnable par variable d'environnement (le stock est xz, donc le noyau le supporte). | spec |
| D-F | Point d'accroche SSH : lancé en arrière-plan depuis `ak_rtsp_wrapper.sh` (option A, §5.T3). | spec |
| D-G | Clés d'hôte : ed25519, générées au premier boot par `dropbearkey` (pas `-R`). | spec |

---

## 2. Versions et empreintes à figer (relevées pendant l'audit)

| Composant | Valeur vérifiée | Remarque |
|---|---|---|
| Dropbear | tag `DROPBEAR_2026.94` (tag léger) → commit `28216cd9af822732a1549b78621c2ea9a76a0fe5`, release du 2026-07-23, la plus récente | le brief disait « 2026.94 » : **existe bien** |
| Archive Dropbear | `https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2` sha256 `e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81` (calculé à l'audit, 2 386 978 octets) | une `.asc` (signature mkj) est publiée à côté ; T2 doit recouper le hash (miroir `dropbear.nl/mirror` ou `git archive` du tag) et valider ou documenter la signature |
| SmolRTSP | `OpenIPC/smolrtsp` commit `8ae3b5115f7a77c383080b51cb7fdba1046f862f` (HEAD de `master`, 2026-09-03 ; = pin `8ae3b51` du flasheur 0.3.2). Dernier tag `v0.2.0` est très ancien : ne pas l'utiliser. | |
| slice99 | `v0.7.8` sha256 de l'archive `07b06b29d06eb74798d85e34b2b50af330f5480e96457bcbfd20f5c6f61d1a30` | URL `https://github.com/hirrolot/slice99/archive/refs/tags/v0.7.8.tar.gz` |
| metalang99 | `v1.13.5` sha256 `f3d1607d76b4b081d3295661c4c2b8d5fde4d5018b1aa409c84fb3a6660ffb90` | idem hirrolot |
| datatype99 | `v1.6.5` sha256 `f38c077afdb91b7d754321be5d3c4a43ed5420c1ad51514d1de20023960f9a8e` | idem |
| interface99 | `v1.0.2` sha256 `8bd007c48cf05436ced60884e8e3a05ede46105f3efae9bf29e0f4d30f938f9e` | idem |
| Flasheur | `4ddict/Action-Smart-Connect-Flasher` v0.3.2 (`VERSION` = 0.3.2) | commit exact à relever par T6b |

Les 4 dépendances SmolRTSP sont exactement celles de `smolrtsp/CMakeLists.txt` (FetchContent). Les sha256 ci-dessus sont ceux d'archives GitHub auto-générées : T1 doit les recalculer et les recouper (GitHub peut changer la compression des archives auto-générées ; si le hash diffère, figer plutôt le commit via `git clone` + `git rev-parse`, voir T1).

---

## 3. Vue d'ensemble : dépendances et vagues

```
Vague 1 (parallèle) :  T1 build natif        ||  T3 init SSH (scripts APP + fixture + .gitignore pour fixture : voir T1)
Vague 2 (parallèle) :  T2 Dropbear (dép. T1) ||  T4 lockdown Telnet/FTP (dép. T3)
Vague 3 (parallèle) :  T5 build_firmware.sh (dép. T1,T2,T3,T4) || T6a script de test matériel (dép. T3,T4)
Vague 4             :  T6b documentation (dép. T1..T6a) 
Optionnel           :  T1b patchs ak_rtsp du flasheur (conditionné à la décision O-1)
```

| Tâche | Branche | Dépend de | Propriétaire exclusif de (résumé) |
|---|---|---|---|
| T1 | `feature/native-linux-build` | — | `src/ak_rtsp/Makefile`, `gen_version.sh`, `gen_version.ps1` (suppr.), `tools/build/*`, `.gitignore` |
| T3 | `feature/ssh-init-scripts` | — | `tools/firmware_patch_templates/*`, `tests/fixtures/app_fake/**` |
| T2 | `feature/dropbear-build` | T1 | `tools/dropbear/*` |
| T4 | `feature/telnet-ftp-lockdown` | T3 | `tools/firmware_patch_templates/camyn_lockdown.sh`, `patch_service_sh.sh`, `audit_remote_services.sh`, `tests/fixtures/app_fake/sbin/service.sh` |
| T5 | `feature/firmware-image-build` | T1,T2,T3,T4 | `src/ak_rtsp/build_firmware.sh`, `tools/flash_scripts/pad_build.sh`, `tests/build_firmware_test.sh` |
| T6a | `feature/hw-check-script` | T3,T4 | `tools/hw_tests/*` |
| T6b | `docs/camyn-v1-install` | tout | `docs/*.md` (nouveaux + mises à jour), `README.md` |
| T1b | `fix/akrtsp-flasher-patches` | décision O-1 | `src/ak_rtsp/{ae,rtsp,night,control}.c` uniquement |

Conflits de fichiers à éviter (déjà résolus dans la répartition) : `.gitignore` appartient à T1 seul (il doit déjà contenir les lignes du build Dropbear, de la toolchain et la **négation pour la fixture**, car `bin/` et `*.bin` sont ignorés par le `.gitignore` actuel et masqueraient `tests/fixtures/app_fake/bin/`). `build_firmware.sh` appartient à T5 seul. Les README/`docs/` n'appartiennent qu'à T6b : T3/T4 documentent dans les en-têtes de leurs scripts et dans leur rapport.

Interface contractuelle entre T1 et T2 (pour qu'ils restent découplés) : T1 fournit `tools/build/env.sh` (à sourcer) qui exporte `ZIG` (chemin absolu du binaire Zig figé), `CAMYN_TARGET=arm-linux-musleabi`, `CAMYN_MCPU=arm926ej_s`, `CAMYN_CC="$ZIG cc -target $CAMYN_TARGET -mcpu=$CAMYN_MCPU"`. T2 ne télécharge pas Zig lui-même.

---

## 4. Contrat de compatibilité avec le flasheur (à respecter par T1, T5)

Lu dans `install.sh` du flasheur 0.3.2 : le flasheur clone `FIRMWARE_REPO` (codé en dur sur FluxAlchemist) au commit `FIRMWARE_COMMIT=3250e0f`, clone SmolRTSP au commit `8ae3b51` dans `src/smolrtsp`, lance `cmake -S smolrtsp -B smolrtsp/build`, **réécrit `gen_version.sh` et applique par `sed`/python quatre patchs sur `ae.c`, `rtsp.c`, `night.c`** (voir O-1), compile avec `make -C src/ak_rtsp ak_rtsp flash_tool 'CC=zig cc -target arm-linux-musleabi -mcpu=arm926ej_s'`, vérifie par `file` (ARM + statically linked), `unsquashfs` le `mtdblock7.bin` sauvegardé, puis appelle :

`APP_EXTRACTED=<arbre dumpé> FW_DIR=<sortie> bash src/ak_rtsp/build_firmware.sh`

et exige un fichier `ak_rtsp_firmware_*.squashfs` de **exactement 5242880 octets** dans `FW_DIR`. Conséquences pour nos tâches :
- Cibles Make `ak_rtsp` et `flash_tool` conservées ; surcharge `CC=` honorée ; `./gen_version.sh` appelé depuis `src/ak_rtsp/` (le flasheur l'écrase par une version équivalente : compatible).
- `build_firmware.sh` garde l'interface `APP_EXTRACTED`, `FW_DIR`, le nom de sortie `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs`, la taille exacte, et ne doit PAS exiger d'outil absent du flasheur (il installe `squashfs-tools`, `cmake`, `python3`, `xz`, etc.).
- Le flasheur ne compile pas Dropbear : `build_firmware.sh` doit trouver un Dropbear déjà construit ou le construire lui-même via `tools/dropbear/build_dropbear.sh` (qui lui-même s'appuie sur la toolchain figée de T1). Le flasheur ne sait PAS intégrer nos modifications par lui-même.
- Le flasheur utilise la porte Telnet 24 de l'ANCIEN firmware (via le hook SD du toolkit `lsc-tuya-toolkit`) pour flasher : cette porte n'existe plus après Camyn (voir §6 et O-2).

---

## 5. Tâches

### T1 — Build natif Linux/Arch et bootstrap des dépendances figées

**Objectif.** Rendre `make` natif sous Arch, sans changer le comportement d'`ak_rtsp`, et fournir un bootstrap reproductible (Zig figé, SmolRTSP + 4 deps figés).

**Fichiers.**
- Créer `src/ak_rtsp/gen_version.sh` (mode 755) : équivalent exact de `gen_version.ps1` (incrémente `build_number.txt`, écrit `version_info.h` avec `BUILD_NUMBER`, `BUILD_TIMESTAMP` `YYYY-MM-DD HH:MM:SS`, `GIT_HASH` via `git describe --always --dirty=-dirty`, repli `unknown`). Même format que celui que le flasheur réécrit (compatibilité).
- Supprimer `src/ak_rtsp/gen_version.ps1`.
- Modifier `src/ak_rtsp/Makefile` : (a) `version.o` appelle `./gen_version.sh` ; (b) `clean` en `rm -f` portable (plus de `pwsh`) ; (c) cible `firmware` = `bash build_firmware.sh` (plus de `wsl`) ; (d) `CC` par défaut inchangé (`zig cc -target arm-linux-musleabi -mcpu=arm926ej_s`, variable `ZIG ?= zig`) mais la surcharge `make CC=...` du flasheur doit continuer de fonctionner ; (e) cible `upload` : remplacer FTP/`curl` (FTP est désactivé en V1, mot de passe `admin` obsolète) par `ssh root@$(CAMERA_IP) 'cat > /tmp/<f> && chmod +x /tmp/<f>' < <f>` (pas de scp : le client OpenSSH ≥ 9 utilise SFTP que Dropbear n'a pas) ; `CAMERA_IP` sans valeur par défaut codée ; (f) `CFLAGS`/`LDFLAGS`/liste de sources INCHANGÉS.
- Créer `tools/build/deps.lock` (format `clé=valeur`, sourcable en sh) : URL + sha256 de l'archive Zig, commit SmolRTSP, tag + sha256 des 4 deps (valeurs §2).
- Créer `tools/build/bootstrap.sh` (bash) : télécharge Zig (`curl -fL`), vérifie sha256, extrait dans `tools/.toolchain/zig-0.14.1-x86_64-linux/` (git-ignoré) ; clone SmolRTSP en `src/smolrtsp` à la révision figée (`git init` + `fetch --depth 1 <commit>` puis `checkout`, vérifier `git rev-parse HEAD`) ; peuple `src/smolrtsp/build/_deps/{slice99,metalang99,datatype99,interface99}-src` depuis les 4 archives vérifiées par sha256 (équivalent du « fetch CMake-less » du README, sans exiger `cmake`). Idempotent (second lancement = no-op rapide), refuse d'écraser un `src/smolrtsp` modifié localement (`git status --porcelain` non vide → erreur claire).
- Créer `tools/build/env.sh` (contrat §3), `tools/build/check_host.sh` (vérifie la présence de `make git curl tar xz bzip2 mksquashfs unsquashfs shellcheck file readelf sha256sum` et affiche la ligne `pacman -S --needed ...` manquante ; ne lance JAMAIS sudo).
- Modifier `.gitignore` : ajouter `/tools/.toolchain/`, `/tools/.cache/`, `/build/`, `/src/ak_rtsp/*.o`, `/src/ak_rtsp/isp_hook.so`, `/src/ak_rtsp/new_app_*.squashfs`, `/dist/`, et la **négation** `!/tests/fixtures/**` (placée après les règles `bin/`, `*.bin`, `*.squashfs`) ; vérifier avec `git check-ignore -v` que `tests/fixtures/app_fake/bin/x` n'est PAS ignoré.

**Branche** `feature/native-linux-build`. **Dépendances** aucune.

**Vérifications locales.**
1. `bash tools/build/check_host.sh` signale correctement un outil manquant (simuler par `PATH` réduit).
2. `bash tools/build/bootstrap.sh` deux fois de suite (2e = no-op) ; altérer volontairement un sha256 dans une copie de `deps.lock` → arrêt avec message.
3. `. tools/build/env.sh && cd src/ak_rtsp && make clean && make ak_rtsp flash_tool` : build OK sans `pwsh`, sans `wsl`.
4. `file src/ak_rtsp/ak_rtsp` → `ELF 32-bit LSB executable, ARM, EABI5 ... statically linked` ; `readelf -h -A src/ak_rtsp/ak_rtsp` → `Class: ELF32`, `Machine: ARM`, attributs `Tag_CPU_name: "ARM926EJ-S"`/`Tag_CPU_arch: v5TE`, absence de `Tag_ABI_VFP_args` / `Tag_FP_arch` (soft-float). Idem `flash_tool`. Reporter la taille (`stat -c%s`) ; ordre de grandeur attendu ≈ 126 KB (README) — signaler tout écart > 20 %.
5. `make -n` ne contient plus `pwsh|wsl|curl.*ftp`. `git diff -- src/ak_rtsp/Makefile` montre `CFLAGS`, `LDFLAGS`, `SRCS` identiques.
6. `shellcheck -s bash` sur les 4 scripts. `git check-ignore` conforme (point .gitignore).
7. Simulation du flasheur : copier `src/ak_rtsp` dans un répertoire temporaire, y écraser `gen_version.sh` par la version du flasheur (heredoc lu dans `install.sh` 0.3.2), relancer `make ak_rtsp flash_tool 'CC=zig cc -target arm-linux-musleabi -mcpu=arm926ej_s'` : doit passer.

**Critères de fin.** Build natif reproductible depuis un clone vierge en 2 commandes (`bootstrap.sh`, `make`) ; `ak_rtsp`/`flash_tool` ARM EABI5 soft-float statiques ; aucun fichier C d'`ak_rtsp` modifié ; rapport mentionnant « compilé, non testé sur matériel ».

---

### T3 — Intégration APP : init SSH (scripts, fixture, clés)

**Objectif.** Fournir les scripts shell qui démarrent Dropbear en root par clé publique au boot, avec persistance dans CONFIG, sans jamais bloquer `ak_rtsp`.

**Comparaison des points d'accroche (analyse faite, recommandation D-F).**

| Option | Principe | Pour | Contre |
|---|---|---|---|
| **A (recommandée)** | `ak_rtsp_wrapper.sh` lance `camyn_ssh.sh start >/dev/null 2>&1 &` AVANT la partie Wi-Fi, puis exécute `ak_rtsp` | fichier que nous possédons en entier (aucun patch par motif sur un script Tuya) ; échec isolé (sous-processus d'arrière-plan, code retour ignoré) ; Dropbear écoute `0.0.0.0` donc n'a pas besoin que l'IP existe ; survit à un crash d'`ak_rtsp` (le démon est détaché) ; même contexte que ce qui est déjà éprouvé matériel | SSH absent si le wrapper n'est jamais lancé (ini/watchdog cassé) — mais alors la caméra est de toute façon hors service (UART disponible) ; le wrapper peut être relancé par le watchdog → garde anti-doublon obligatoire |
| B | entrée `[process_1]` dans `_ht_process.ini` | supervision par le watchdog | comportement du watchdog stock inconnu (relance ? redémarrage matériel si un processus meurt ou si le binaire manque ?) ; Dropbear se démonise (le watchdog verrait une sortie immédiate → boucle de relance) sauf `-F` ; ajoute un risque sur LE fichier qui lance `ak_rtsp` ; contredit « un échec de Dropbear ne doit jamais affecter ak_rtsp » tant que le watchdog n'est pas lu sur un vrai dump |
| C | ligne ajoutée dans `service.sh` (`system_run_init`) | démarrage le plus précoce ; indépendant du wrapper | patch par motif sur un script Tuya inconnu (absent du dépôt) ; une erreur de syntaxe/blocage dans un script lancé tôt peut stopper le boot ; `service.sh` est copié en `/tmp` et réécrit à chaque version |

Recommandation : **A**. L'option C reste un complément possible UNIQUEMENT pour la tâche T4 (retirer telnetd/ftpd), pas pour SSH. Ne pas utiliser B tant que le fonctionnement du watchdog n'est pas lu dans le dump réel.

**Fichiers.**
- Modifier `tools/firmware_patch_templates/ak_rtsp_wrapper.sh` : ajout, en tête (après `PATH=`), d'un bloc court : si `/usr/sbin/camyn_ssh.sh` existe et est exécutable, `( /usr/sbin/camyn_ssh.sh start >/dev/null 2>&1 & )` ; idem pour `/usr/sbin/camyn_lockdown.sh` si présent (fourni par T4, optionnel à l'exécution) ; le reste du wrapper (Wi-Fi, `exec`/code retour d'`ak_rtsp`) est INCHANGÉ. Aucun `set -e`, aucun `wait`.
- Créer `tools/firmware_patch_templates/camyn_ssh.sh` (installé en `/usr/sbin/camyn_ssh.sh`, mode 755), POSIX sh, sous-commandes `start` (défaut) et `status`. Comportement de `start` :
  1. garde anti-doublon : `mkdir /tmp/camyn_ssh.lock` atomique (sinon sortir 0) ; vérifier aussi `/tmp/dropbear.pid` + `kill -0` ; si dropbear tourne déjà, sortir 0.
  2. si `/usr/bin/dropbear` ou `dropbearkey` est absent/non exécutable : journaliser et sortir 0 (jamais d'erreur bloquante).
  3. `mkdir -p /etc/config/dropbear` ; `chmod 700`. Attendre que `/etc/config` soit monté en écriture (boucle bornée, p. ex. 30 × 1 s, test `touch` d'un fichier témoin) ; si jamais inscriptible : démarrer quand même avec une clé d'hôte éphémère dans `/tmp` ET aucune clé autorisée persistante (journaliser).
  4. clé d'hôte : si `/etc/config/dropbear/dropbear_ed25519_host_key` absente → `dropbearkey -t ed25519 -f <tmp dans le même répertoire>` puis `mv` atomique + `sync`. (Entropie : le noyau 4.4 peut bloquer `getrandom` tôt après le boot sur ARM sans RTC ; le script tourne déjà en arrière-plan, donc sans conséquence pour `ak_rtsp` ; ne pas ajouter de `timeout` non garanti.)
  5. `authorized_keys` : (a) provisionnement SD — si `/mnt/camyn/authorized_keys` existe (SD montée par le stock sur `/mnt` ; chemin à confirmer sur matériel, rendre le dossier SD configurable en tête de script), valider chaque ligne non vide/non commentaire (types acceptés `ssh-ed25519` uniquement en V1, voir O-3 ; refuser toute ligne contenant `command=` sauf option autorisée explicite), puis REMPLACER `/etc/config/dropbear/authorized_keys` de façon atomique si le contenu diffère (`cmp`), `chmod 600`, `chown 0:0` ; ne jamais supprimer le fichier sur la SD ; (b) clé par défaut embarquée — si `/etc/config/dropbear/authorized_keys` n'existe pas ou est vide ET que `/usr/local/camyn/authorized_keys.default` existe, la copier. Aucune clé = Dropbear démarre quand même (inutile mais inoffensif) et le journal le dit.
  6. permissions exigées par Dropbear (`checkpubkeyperms`, lu dans le source 2026.94) : le fichier et chaque composant du chemin jusqu'à `/` doivent appartenir à root/utilisateur et ne pas être inscriptibles par groupe/autres ; donc `/etc/config/dropbear` 0700, `authorized_keys` 0600, `umask 077` pendant les écritures ; vérifier et journaliser (`stat -c '%a %u'`) le mode de `/etc/config`, `/etc`, `/`.
  7. démarrer : `/usr/bin/dropbear -r /etc/config/dropbear/dropbear_ed25519_host_key -D /etc/config/dropbear -p 22 -P /tmp/dropbear.pid -T 3 -E 2>>/tmp/camyn_ssh.log` (le démon se détache). `-D` rend inutile le `$HOME` de root (pour mémoire : `/` d'après `docs/flash_partition_map_and_rootfs.md` ; ROOTFS en lecture seule). Ne PAS passer `-s -g -j -k -w` : ils sont retirés/inopérants dans notre build (auth par mot de passe et redirections désactivées à la compilation) et `-w` interdirait root. Limiter la taille de `/tmp/camyn_ssh.log` (tronquer au démarrage ; rotation inutile).
  8. journal d'une ligne par étape (`camyn_ssh: ...`) sur `/tmp/camyn_ssh.log` ; aucun secret journalisé.
  - Boucle de supervision : NON dans `camyn_ssh.sh` en V1 (un process sh résident de plus, et risque de relance en boucle). Dropbear est un démon simple ; si le test matériel montre des morts de Dropbear, ajouter une supervision en V1.1.
- Créer `tests/fixtures/app_fake/` : arbre APP SYNTHÉTIQUE minimal représentatif (`bin/anyka_ipc` fichier factice de 4 Kio, `bin/cmdserver`, `bin/msgserver`, `sbin/main.sh`, `sbin/service.sh` (synthétique, contenant des lignes `telnetd`, `tcpsvd ... ftpd`, un hook SD de style `hostapd`/`hack.sh` — écrites de mémoire à partir de la documentation publique, avec en-tête « SYNTHETIC FIXTURE »), `local/_ht_process.ini` à 3 entrées, `local/_ht_sw_settings.ini` minimal, `lib/`, un lien symbolique, un fichier exécutable et un non exécutable) + `tests/fixtures/README.md` indiquant « synthetic, no vendor content ».
- Créer `tests/ssh_init_test.sh` (bash hôte) : exécute `camyn_ssh.sh` dans un faux environnement (variables `CAMYN_ROOT` surchargeant `/etc/config`, `/tmp`, `/usr/bin` vers un répertoire temporaire, avec de faux `dropbear`/`dropbearkey` en shell) pour tester : premier boot, second boot (idempotent), doublon concurrent, binaire absent, `/etc/config` non inscriptible, SD avec clés valides/invalides/avec `command=`, permissions résultantes. Pour que ce soit possible, `camyn_ssh.sh` lit ses chemins dans des variables en tête de script avec valeurs par défaut réelles.

**Branche** `feature/ssh-init-scripts`. **Dépendances** aucune (utilise des faux binaires ; le vrai Dropbear vient de T2).

**Vérifications locales.** `shellcheck -s sh tools/firmware_patch_templates/camyn_ssh.sh tools/firmware_patch_templates/ak_rtsp_wrapper.sh` propre ; `dash -n` (si présent) et `busybox sh -n` (si présent) pour syntaxe ; `bash tests/ssh_init_test.sh` passe tous les scénarios ; diff du wrapper : seule l'insertion du bloc de démarrage ; `git check-ignore` ne masque aucun fichier de la fixture (`git status` les liste).

**Critères de fin.** Scénarios de test verts ; wrapper strictement rétro-compatible (sans `camyn_ssh.sh` ni dropbear, il se comporte comme avant) ; documentation dans l'en-tête de `camyn_ssh.sh` (rôle, chemins, permissions, journal) ; rapport listant les points « à confirmer sur matériel » (chemin de montage SD, mode de `/etc/config`, entropie, `/dev/pts`).

---

### T2 — Build Dropbear statique minimal

**Objectif.** Script reproductible qui produit un binaire multi-appel `dropbearmulti` ARM soft-float statique, minimal, avec versions et empreintes figées, et un rapport de taille.

**Fichiers (tous nouveaux).**
- `tools/dropbear/version.lock` : tag `DROPBEAR_2026.94`, commit `28216cd9af822732a1549b78621c2ea9a76a0fe5`, URL de l'archive + sha256 (§2).
- `tools/dropbear/localoptions.h` : options de `default_options.h` à surcharger (noms vérifiés dans le source 2026.94) :
  `DROPBEAR_SVR_PASSWORD_AUTH 0`, `DROPBEAR_SVR_PAM_AUTH 0` (déjà 0), `DROPBEAR_X11FWD 0` (déjà 0), `DROPBEAR_SVR_AGENTFWD 0`, `DROPBEAR_CLI_AGENTFWD 0`, `DROPBEAR_SVR_LOCALTCPFWD 0`, `DROPBEAR_SVR_REMOTETCPFWD 0`, `DROPBEAR_RSA 0`, `DROPBEAR_ECDSA 0`, `DROPBEAR_DSS 0` (déjà 0), `DROPBEAR_SK_KEYS 0`, `DROPBEAR_ED25519 1`, `DROPBEAR_DH_GROUP14_SHA256 0` (garder `DROPBEAR_CURVE25519 1`), `DROPBEAR_SHA1_HMAC 0`, `DROPBEAR_3DES 0`, `DROPBEAR_ENABLE_CBC_MODE 0`, `DROPBEAR_ENABLE_GCM_MODE 0`, `DROPBEAR_CHACHA20POLY1305 1`, `DROPBEAR_AES128 1`/`AES256 1` avec `ENABLE_CTR_MODE 1` (compatibilité clients), `DO_MOTD 0`, `DROPBEAR_SMALL_CODE 1`, `DROPBEAR_USE_PASSWORD_ENV 0`, `DROPBEAR_DELAY_HOSTKEY` laissé (permet `-R` en secours). Chaque ligne commentée en anglais. T2 vérifie à la compilation que le binaire n'accepte plus `-s` (option mot de passe) : `dropbear -h` ne doit pas la lister ; vérifier chaque macro par `grep` dans le `config.h` généré/`options.h`. Si une macro est refusée par `sanity.h` (dépendances), ajuster et le noter.
- `tools/dropbear/build_dropbear.sh` (bash hôte) : source `tools/build/env.sh` ; télécharge l'archive (cache `tools/.cache/`), **vérifie sha256**, recoupe avec le hash du tag via `.asc` si `gpg` présent ou avec le contenu `git archive` du tag (sinon avertit explicitement) ; extrait dans un répertoire de build hors arbre (`build/dropbear/`) ; copie `localoptions.h` ; `./configure --host=arm-linux-musleabi --disable-zlib --disable-pam --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline --disable-pututxline --enable-static` avec `CC="$CAMYN_CC"`, `AR="$ZIG ar"`, `RANLIB="$ZIG ranlib"` (enveloppes `zig` à fournir comme petits scripts dans le répertoire de build car autoconf attend un nom de programme simple), `CFLAGS="-Os -mfloat-abi=soft -ffunction-sections -fdata-sections"`, `LDFLAGS="-static -Wl,--gc-sections"`, `LTM_CFLAGS=-Os` (pas de `--disable-bundled-libtom`) ; puis `make PROGRAMS="dropbear dropbearkey" MULTI=1` (variante avec `scp` + `SCPPROGRESS=0` mesurée à part, voir ci-dessous) ; produit `dist/dropbear/dropbearmulti` (strippé via `zig objcopy --strip-all` ou `llvm-strip` fourni par Zig, au choix documenté) + `dist/dropbear/BUILDINFO.txt` (tag, commit, sha256 archive, options, version Zig, sha256 du binaire, taille avant/après strip).
- `tools/dropbear/size_report.sh` : compare les variantes (a) `dropbear dropbearkey` multi, (b) idem + `scp`, (c) binaires séparés ; affiche tailles et gain. Règle : inclure `scp` seulement si le surcoût ≤ 40 Kio ; sinon exclure. (Le client OpenSSH moderne a besoin de `scp -O` face à Dropbear ; l'alternative `ssh host 'cat > f' < f` est documentée par T6b et utilisée par la cible `make upload` de T1.)
- Pas de patch du source Dropbear : si un patch devenait nécessaire pour zig/musl, STOP et le signaler (décision utilisateur).

**Branche** `feature/dropbear-build`. **Dépendances** T1 (`tools/build/env.sh`, Zig local).

**Vérifications locales.**
1. Build depuis un répertoire propre OK ; relancer = même sha256 du binaire non strippé (reproductibilité : tester deux builds consécutifs ; si non reproductible, identifier la cause — `__DATE__`/chemins — et appliquer `-ffile-prefix-map`).
2. `file dist/dropbear/dropbearmulti` → ARM, EABI5, statically linked ; `readelf -h -A` → ARM, v5TE, pas d'attribut VFP/hard-float ; `readelf -d` sans `NEEDED` ; `strings dropbearmulti | grep -c 'password'` documente ce qui reste ; `dropbearmulti dropbear -h` n'affiche pas `-s`/`-g`/`-B` ; `dropbearmulti dropbearkey -t ed25519 -f` fonctionne sous `qemu-arm` UNIQUEMENT si qemu-user est déjà installé (sinon « non exécuté »).
3. Rapport de taille (brut, strippé, tailles des variantes avec/sans scp). Ordre de grandeur attendu 200–400 Kio (hypothèse à confirmer par la mesure) ; signaler si > 450 Kio.
4. `shellcheck -s bash` propre ; aucune écriture hors `build/`, `dist/`, `tools/.cache/`.

**Critères de fin.** Un binaire reproductible + `BUILDINFO.txt` + rapport de taille ; licence Dropbear (`LICENSE`) copiée dans `dist/dropbear/LICENSE.dropbear` (incl. licences libtomcrypt/libtommath, domaine public) pour distribution avec l'image ; mention « non exécuté sur matériel ».

---

### T4 — Désactivation Telnet/FTP depuis APP + interrupteur de maintenance

**Objectif.** Faire en sorte que telnet (23, 24) et FTP (21) ne tournent pas en fonctionnement normal, sans modifier ROOTFS, sans réactivation par le watchdog ni la reconnexion réseau ; offrir un interrupteur de maintenance explicite.

**Ce que les sources externes établissent (voir §8) :**
- Sur ce firmware (Tuya/Anyka, boot `init → rcS → rc.local → /usr/sbin/main.sh → /tmp/service.sh start`), `telnetd` et `ftpd` sont démarrés par `system_run_init` de **`/usr/sbin/service.sh`, fichier de la partition APP** (source : `berobloom/LSC-1080p-Anyka-Decloud docs/HARDWARE.md`/`LOGBOOK.md` : « `telnetd`/`ftpd` start every boot », sur un modèle voisin HT-IPC280, firmware 6.2802 ; le toolkit `tasarren/lsc-tuya-toolkit` cite 6.2863 pour la même chaîne). Le toolkit décrit aussi que le mode « factory test » d'`anyka_ipc` ouvre telnet:23 ; `anyka_ipc` étant supprimé de Camyn, ce chemin disparaît. `_ht_av_mode.conf` sur la SD démarre un FTP.
- **Telnet 24** : n'est PAS un service du firmware stock ; c'est le démon lancé par le hook SD du toolkit (`custom/scripts/telnet.sh`, `TELNET_PORT=24`, boucle de relance toutes les 1–2 s, `telnetd -p 24 -l /bin/sh` SANS authentification) que le flasheur configure. Ce hook s'exécute parce que le `service.sh` stock, voyant `_ht_ap_mode.conf` sur la SD, exécute le script `hostapd` de la SD (donc `hack.sh`) en root. **Conséquence : une SD laissée dans la caméra après l'installation par le flasheur rouvre un telnet:24 sans mot de passe à chaque boot, indépendamment de notre APP, tant que `service.sh` garde ce comportement.** Il faut donc neutraliser ce hook dans `service.sh` (sauf mode maintenance) ET documenter de retirer la SD du flasheur/ses marqueurs.
- `service.sh` du firmware réel de cette caméra (D26228 attendu) n'est pas disponible : les motifs exacts sont des HYPOTHÈSES à confirmer sur le dump.

**Fichiers.**
- `tools/firmware_patch_templates/patch_service_sh.sh` (hôte, bash) : usage `patch_service_sh.sh <app_tree>` ; modifie `<app_tree>/sbin/service.sh` de façon déterministe et idempotente : (1) neutralise (commente avec marqueur `# CAMYN-DISABLED:`) toute ligne qui lance `telnetd`, `utelnetd`, `tcpsvd ... ftpd`, `ftpd`, `bftpd`, `vsftpd` ; (2) enveloppe le chemin d'exécution du script SD (`hostapd`/`hack.sh` / `_ht_ap_mode.conf`) pour qu'il ne s'exécute que si `/mnt/CAMYN_MAINTENANCE` existe (voir interrupteur) ; (3) AUCUNE autre ligne modifiée. Échoue (code ≠ 0, message clair) si le fichier est absent ; si AUCUN motif telnet/ftp n'est trouvé, n'échoue pas mais écrit un avertissement fort et exit code 0 distinct (3) que `build_firmware.sh` (T5) traitera comme « avertissement bloquant sauf `CAMYN_ALLOW_UNPATCHED_SERVICE=1` ». Écrit un diff dans un fichier de rapport. Le script ne s'appuie que sur `grep/sed/awk` POSIX.
- `tools/firmware_patch_templates/audit_remote_services.sh` (hôte) : usage `audit_remote_services.sh <app_tree>` ; grep dans TOUT l'arbre APP (scripts shell, `.ini`, binaires via `strings`) pour `telnetd|utelnetd|tcpsvd|ftpd|bftpd|vsftpd|dropbear|sshd|httpd|lighttpd|:23|:21|port.*24` et liste précise fichier:ligne ; produit `audit_remote_services.txt`. Sert de preuve sur le vrai dump (le build l'archive à côté de l'image).
- `tools/firmware_patch_templates/camyn_lockdown.sh` (installé `/usr/sbin/camyn_lockdown.sh`, POSIX sh, 755) : filet de sécurité d'exécution, lancé en arrière-plan par le wrapper (T3). Comportement : (1) garde anti-doublon ; (2) si `/mnt/CAMYN_MAINTENANCE` existe → journaliser « maintenance mode: telnet not touched », ne rien tuer, ne rien démarrer, sortir ; (3) sinon, tuer les `telnetd`/`utelnetd` et `tcpsvd`/`ftpd` présents (via `ps` + `kill`, sans `pkill` ni `killall` non garantis ; tester leur présence) ; (4) boucle de surveillance BORNÉE : toutes les 5 s pendant 2 minutes après le boot (couvre la fenêtre de démarrage tardif), puis toutes les 60 s ; arrêt propre si la maintenance apparaît ; consommation négligeable (un seul sh en `sleep`) — le coût RAM est mesuré par T6a ; (5) ne touche JAMAIS à `ak_rtsp`, `dropbear`, `wpa_supplicant`, `cmdserver`, `msgserver` (liste d'exclusion testée) ; (6) n'écoute ni ne se connecte au réseau.
- Interrupteur de maintenance : fichier vide `CAMYN_MAINTENANCE` à la racine de la microSD (montée en `/mnt` par le stock ; à confirmer). Présent → (a) `service.sh` patché laisse le hook SD du toolkit s'exécuter (ce qui peut rouvrir telnet:24 via `hack.sh` si la SD le configure), (b) `camyn_lockdown.sh` ne tue rien. Absent → tout est fermé. Jamais de réactivation par le watchdog ni par un événement réseau : seule la présence physique de la SD marquée active le mode, et seulement au boot ou tant que la SD est montée. Documenter que cela revient à un accès physique (même niveau de confiance que le hook SD stock).
- Modifier `tests/fixtures/app_fake/sbin/service.sh` (synthétique, déjà créé par T3) pour exercer plusieurs variantes de lancement (ligne simple, ligne en arrière-plan `&`, dans une fonction, via variable) → placer les variantes dans `tests/fixtures/app_fake_variants/{a,b,c}/sbin/service.sh`.
- `tests/lockdown_test.sh` : tests hôte avec faux `ps`/`kill` et variables de chemin surchargées ; tests de `patch_service_sh.sh` sur les variantes (résultat attendu, idempotence = 2e passage sans diff, `bash -n`/`sh -n` du résultat).

**Fenêtre d'exposition (à documenter dans le rapport, puis par T6b).**
- Si le démarrage de telnetd/ftpd est bien dans `service.sh` (hypothèse la plus probable) et que le patch est appliqué : fenêtre = **aucune** (jamais lancé).
- Si seul le filet d'exécution agit (patch sans effet) : fenêtre = du lancement par le stock jusqu'au premier passage de `camyn_lockdown.sh` (wrapper lancé par le watchdog quelques secondes après `main.sh`) — de l'ordre de la dizaine de secondes (estimation, non mesurée) ; mesure à faire par T6a/UART (`ss`/`netstat` en boucle depuis le boot).
- Si telnetd/ftpd sont lancés depuis ROOTFS (`/etc/init.d/rcS`, `inittab`) : fenêtre = jusqu'au wrapper, et un `rc.local` pourrait les relancer ; **aucun correctif sans modifier ROOTFS** (interdit en V1 sans validation) → voir O-4.

**Branche** `feature/telnet-ftp-lockdown`. **Dépendances** T3 (contrat du wrapper, fixture).

**Vérifications locales.** `shellcheck -s sh` sur `camyn_lockdown.sh` ; `shellcheck -s bash` sur les scripts hôte ; `bash tests/lockdown_test.sh` : variantes patchées, idempotence, maintenance présente/absente, exclusions respectées ; le diff `service.sh` avant/après ne change aucune autre ligne ; le wrapper n'est PAS modifié par T4.

**Critères de fin.** Patch + filet + interrupteur testés sur fixtures (« testé sur fixture, non testé sur matériel ») ; rapport listant exactement les motifs recherchés et ce qu'il faudra vérifier sur le vrai `service.sh` (commande `bash audit_remote_services.sh app_extracted`).

---

### T5 — `build_firmware.sh` : intégration, compression, garde-fous

**Objectif.** Réécrire `build_firmware.sh` pour construire l'image APP complète (ak_rtsp + Dropbear + scripts + lockdown) à partir d'un arbre `app_extracted/` (ou de la fixture), de façon reproductible et sûre.

**Fichiers.** `src/ak_rtsp/build_firmware.sh` (réécriture), `tools/flash_scripts/pad_build.sh` (remplacer `head|tr` par `dd`/`printf` ; garder nom de fichier et taille 5242880 ; garder l'interface `pad_build.sh <squashfs>`), `tests/build_firmware_test.sh`.

**Comportement attendu.**
1. Interface conservée (§4) : `APP_EXTRACTED`, `FW_DIR`, sortie `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` = 5242880 octets. Nouvelles variables : `CAMYN_SQUASH_COMP` (`gzip` défaut | `xz`), `CAMYN_AUTHORIZED_KEYS_DEFAULT` (chemin d'un fichier de clés PUBLIQUES à embarquer, optionnel, voir O-5), `DROPBEAR_MULTI` (binaire précompilé ; sinon appeler `tools/dropbear/build_dropbear.sh`), `CAMYN_ALLOW_UNPATCHED_SERVICE`, `KEEP_PATCHED=1` (réutiliser `app_patched/` ; défaut = TOUJOURS repartir de zéro pour éviter qu'un ancien patch fige l'image), `SOURCE_DATE_EPOCH` (date de construction fixée pour la reproductibilité).
2. Étapes : (a) contrôle d'entrée : `APP_EXTRACTED` existe, contient `bin/`, `sbin/`, `local/` ; refuser un arbre qui contient déjà `bin/dropbear` d'une exécution précédente sans `KEEP_PATCHED` ; (b) copie `cp -a` dans `app_patched/` ; (c) `rm bin/anyka_ipc` (comme avant) ; (d) installer `ak_rtsp`, `ak_rtsp_wrapper.sh`, `_ht_process.ini` (comme avant) ; (e) installer `dropbearmulti` en `/usr/bin/dropbearmulti` + liens symboliques relatifs `dropbear`, `dropbearkey` (+`scp` si T2 l'a inclus) ; `camyn_ssh.sh` et `camyn_lockdown.sh` en `sbin/` (755) ; (f) `patch_service_sh.sh` puis `audit_remote_services.sh` (archivé à côté de l'image) ; (g) `local/camyn/MANIFEST.txt` dans l'image (version Camyn, git describe, versions Dropbear/SmolRTSP/Zig, sha256 de `ak_rtsp` et de `dropbearmulti`, date) et licences tierces (`LICENSE.dropbear`) ; clés par défaut éventuelles en `local/camyn/authorized_keys.default` (0644, publiques) ; (h) **contrôles de permissions** sur l'arbre patché : scripts 755, `dropbearmulti` 755, aucun fichier inscriptible par groupe/autres dans `sbin`, `bin`, `local/camyn`, aucune clé privée (`grep -l "BEGIN .*PRIVATE KEY"` + fichiers `*_host_key`) → échec si trouvée ; (i) `mksquashfs` avec `-comp $COMP -b 131072 -all-root -noappend -no-xattrs -no-progress -mkfs-time $SOURCE_DATE_EPOCH -all-time $SOURCE_DATE_EPOCH` (pour xz : `-Xbcj arm` à tester) ; (j) round-trip : `unsquashfs` dans un répertoire `mktemp` (pas `/tmp/rt_check` fixe), comparaison par `diff -r` PLUS comparaison des modes, des liens symboliques et du sha256 par fichier (la comparaison par md5 seule de l'existant ne vérifie ni modes ni liens) ; `unsquashfs -s` pour vérifier compression et taille de bloc ; (k) **garde-fous** : taille de l'image squashfs avant remplissage ; refus (exit ≠ 0) si > 5242880 ; refus si > 5242880 − 131072 (marge d'un bloc) sauf `CAMYN_ALLOW_TIGHT=1`, avec message ; (l) padding 0xFF à 5242880 exactement via `pad_build.sh` ; vérification : taille exacte, les `N` premiers octets == squashfs, le reste uniquement 0xFF (`cmp`/`tail | tr`) ; (m) rapport `build_report.txt` à côté de l'image : compression, taille squashfs, espace libre (octets et %), taille de chaque binaire ajouté, 10 plus gros fichiers de l'image, sha256 de l'image, sha256 des entrées, avertissements ; (n) fichier `*.sha256` à côté de l'image (format `sha256sum`).
3. Aucune écriture dans le dépôt hors `FW_DIR` ; `trap` pour nettoyer les répertoires temporaires.
4. Si `mksquashfs` n'accepte pas l'une des options (version d'outil) : échec explicite, pas de repli silencieux.

**Branche** `feature/firmware-image-build`. **Dépendances** T1 (ak_rtsp, env), T2 (dropbearmulti), T3 (scripts, fixture), T4 (lockdown/patch).

**Vérifications locales (fixture uniquement).**
1. `bash tests/build_firmware_test.sh` : construit l'image avec `APP_EXTRACTED=tests/fixtures/app_fake` ; attend : exit 0, image de 5242880 octets, nom conforme, `.sha256` valide (`sha256sum -c`), `unsquashfs -s` indique gzip/131072 ; `unsquashfs -d` puis présence de `bin/ak_rtsp`, `bin/dropbearmulti`, liens, modes ; absence de `bin/anyka_ipc` ; `local/_ht_process.ini` à une seule entrée `ak_rtsp_wrapper.sh` ; `sbin/service.sh` patché ; `audit_remote_services.txt` produit.
2. Deux constructions successives avec le même `SOURCE_DATE_EPOCH` → même sha256 du contenu squashfs (avant nom horodaté).
3. Scénarios d'échec : fixture gonflée (fichier aléatoire pour dépasser 5 Mio) → refus ; arbre sans `sbin/` → refus ; clé privée plantée → refus ; binaire `ak_rtsp` absent → refus clair ; `CAMYN_SQUASH_COMP=xz` → image valide (tailles gzip vs xz comparées et reportées).
4. Compatibilité flasheur : appel exact `APP_EXTRACTED=… FW_DIR=… bash src/ak_rtsp/build_firmware.sh` depuis un autre répertoire courant ; le fichier est trouvé par le motif `ak_rtsp_firmware_*.squashfs`.
5. `shellcheck -s bash` propre.

**Critères de fin.** Tous les tests ci-dessus verts ; rapport de taille sur fixture clairement étiqueté « fixture, non représentatif de l'espace libre réel » (la vraie marge ne se connaît qu'avec le dump) ; aucune affirmation de validité matérielle.

---

### T6a — Script de test matériel post-flash (à lancer en SSH)

**Objectif.** Un seul script POSIX, lisible, exécuté sur la caméra par l'utilisateur après flash, qui collecte des preuves pour cocher les critères d'acceptation.

**Fichiers.** `tools/hw_tests/camyn_hwcheck.sh` (installé aussi dans l'image sous `/usr/sbin/camyn_hwcheck.sh`? NON en V1 : copié par l'utilisateur via `ssh root@cam 'cat > /tmp/hwcheck.sh' < ...` pour ne pas toucher `build_firmware.sh` ; T5 ne le référence pas), `tools/hw_tests/README-header` (en-tête de script seulement), `tests/hwcheck_test.sh` (hôte, faux `ss`/`netstat`/`ps`).

**Comportement.** Sortie texte + code retour 0/1 + lignes `PASS|FAIL|WARN|INFO` : (1) ports en écoute via `ss -ltn` sinon `netstat -ltn` sinon lecture de `/proc/net/tcp` (BusyBox minimal : l'implémentation `/proc/net/tcp` est le repli garanti) : attendus 22, 554, 8091 présents ; 23, 24, 21, 80 absents (80 : « aucun service Camyn » = WARN si présent) ; 8091 : lier à `127.0.0.1`/LAN uniquement = INFO de l'adresse d'écoute ; (2) processus présents/absents par `ps` (`ak_rtsp`, `dropbear`, `wpa_supplicant`, `udhcpc` ; absents : `anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd`) ; (3) `free`/`/proc/meminfo` (MemFree, Cached) et RSS de `ak_rtsp`/`dropbear` depuis `/proc/<pid>/status` ; (4) `df` des montages `/usr` (APP) et `/etc/config`, `mount | grep -E 'usr|etc/config'` (squashfs, jffs2 rw) ; `cat /proc/mtd` comparé à la table attendue ; (5) fichiers : `/etc/config/dropbear/*` modes/propriétaires, `authorized_keys` non vide (nombre de lignes, jamais le contenu des clés privées ; l'empreinte publique via `dropbearkey -y` autorisée) ; (6) `cat /tmp/camyn_ssh.log` (dernières lignes) ; (7) `uname -a`, `cat /proc/version`, `/proc/cpuinfo` (ARMv5TE), `dmesg | tail` filtré SQUASHFS/jffs2/watchdog ; (8) charge CPU (`uptime`, `top -bn1 | head`) ; (9) rappel imprimé des tests manuels restants (micro, nuit, 8091 LIST/GET/SET, coupure d'alimentation) sans les automatiser ; option `--boot-window` : boucle de ~120 s qui échantillonne les ports 21/23/24 toutes les secondes pour mesurer la fenêtre d'exposition (écrit un horodatage relatif en secondes depuis `/proc/uptime`).

**Branche** `feature/hw-check-script`. **Dépendances** T3, T4 (noms de fichiers/journaux). **Vérifications** `shellcheck -s sh`, `bash tests/hwcheck_test.sh` (faux binaires), pas de dépendance à `awk` exotique ni à `bash`.

**Critères de fin.** Test hôte vert ; le script n'écrit rien en flash ni dans `/etc/config` ; rapport indiquant « jamais exécuté sur matériel ».

---

### T6b — Documentation (en anglais, dans `docs/` et `README.md`)

**Objectif.** Documenter l'installation de Camyn V1 pour un utilisateur Arch Linux, sans mentir sur l'état de validation.

**Fichiers.** Créer `docs/camyn_v1_install.md` ; mettre à jour `docs/building_your_own_firmware.md`, `docs/firmware_building_and_flashing.md`, `docs/flash_partition_map_and_rootfs.md` (supprimer les affirmations devenues fausses : Windows/WSL/PowerShell, `make` Windows, FTP `curl`, backdoor `passwd` Telnet, « gzip vs XZ »), `README.md` (section Windows → Arch ; lien vers la nouvelle doc ; statut de test), `docs/hardware_and_uart.md` (ajout section UART récupération Camyn si utile). Ne pas toucher aux anciennes captures/images.

**Contenu obligatoire, dans cet ordre.**
1. Avertissement « non validé sur matériel » + tableau « testé sur fixture / compilé / à tester sur la caméra » (liste des critères d'acceptation du brief, cases vides à cocher par l'utilisateur).
2. **Sauvegarde des 8 partitions EN PREMIER** (avant toute autre étape) : procédure de dump `mtdblock0..7`, vérification des tailles exactes (204800, 4096, 4096, 65536, 1572864, 1032192, 262144, 5242880), `sha256sum` sur la caméra (`sha256sum` BusyBox si présent, sinon `md5sum` + calcul SHA-256 côté PC), copie hors SD, conservation de plusieurs exemplaires ; rappel : le dump contient des secrets de l'utilisateur (Wi-Fi, identifiants Tuya dans CONFIG) → jamais dans Git. Mentionner que le flasheur fait aussi cette sauvegarde (étape stage 1, MD5) mais que l'utilisateur doit la valider lui-même.
3. Identification matérielle (réf. 3215672.3, SI attendu D26228 : « à confirmer sur l'étiquette ») ; vérification de `/proc/mtd` contre la table attendue.
4. Installation des outils hôte (`pacman -S --needed squashfs-tools cmake shellcheck ...`), `tools/build/bootstrap.sh`, build ak_rtsp, build Dropbear, génération de la paire de clés SSH ed25519 de l'utilisateur (clé privée jamais copiée dans le dépôt/l'image).
5. Construction de l'image à partir DU dump de l'utilisateur (`unsquashfs` du `mtdblock7.bin`, lecture du rapport de taille et de l'audit des services).
6. Flashage : procédure existante `flash_tool` sur `/dev/mtd7` (rappel du piège DMA, `dd` interdit), vérification du journal, mise hors tension manuelle (le `reboot` logiciel est peu fiable) ; **séquence de première connexion SSH** et provisionnement de `authorized_keys` (SD ou clé embarquée).
7. Utilisation du flasheur avec le fork : voir O-2 ; documenter la méthode retenue, les limites (patchs automatiques du flasheur sur le source, commit à pousser pour pouvoir être cloné, Telnet 24 utilisé uniquement avant le premier flash), le risque SD (hook toolkit qui rouvre telnet:24) et la consigne de retirer/vider la SD du flasheur avant usage normal.
8. Restauration : `restore.sh` (APP seul, depuis UART/`init=/bin/sh` comme dit dans le script) et `restore_full_firmware.sh` ; **restauration hors réseau** par microSD + UART ; rappel qu'on ne reconstruit que l'APP, donc U-Boot atteignable en cas d'APP défectueuse (source : docs SAFETY du dépôt Decloud) ; procédure de réflashage depuis SSH (image sur SD ou via `ssh`/`cat` vers `/tmp` (RAM 64 Mio) puis `flash_tool`).
9. UART : brochage et paramètres existants (renvoyer à `hardware_and_uart.md`), accès U-Boot, `init=/bin/sh`.
10. Mode maintenance (`CAMYN_MAINTENANCE` sur la SD), fenêtre d'exposition réelle selon l'audit, ce qu'il advient du Telnet 24 du flasheur.
11. Rapport de ressources : modèle de tableau à remplir avec la sortie de `camyn_hwcheck.sh` (RAM libre avant/après, RSS Dropbear, taille image, espace libre APP) ; les valeurs de la fixture NE sont PAS publiées comme valeurs réelles.
12. Liste « tests faits vs tests matériels restants », mise à jour par chaque agent via son rapport.
13. Licences : Dropbear (MIT-like, texte joint), libtomcrypt/libtommath (domaine public), SmolRTSP, Zig (non redistribué).

**Branche** `docs/camyn-v1-install`. **Dépendances** T1–T6a (relire les rapports). **Vérifications** : toutes les commandes citées existent dans le dépôt (script `grep` des chemins), liens relatifs valides, aucune mention de Windows comme prérequis, aucun mot de passe/clé/IP privée, texte entièrement en anglais, aucun « validated/verified on hardware ». **Critères de fin** : un lecteur peut suivre sauvegarde → build → flash → restauration sans autre source.

---

### T1b — (conditionnelle) Intégration au source des 4 correctifs appliqués par le flasheur

À ne lancer que si l'utilisateur répond « oui » à O-1. Le flasheur 0.3.2 modifie à la volée `ae.c` (`.exp_max 1039 → 2`), `rtsp.c` (suppression de `O_NONBLOCK` sur le socket client : corrige des macroblocs H.264 corrompus en RTSP/TCP), `night.c` (seuils `NIGHT_TRIGGER_HW_EXP 1800→3000`, `DAY_TRIGGER_HW_EXP 800→1200`) et le code d'override jour/nuit protégé par mutex (4e patch : texte du script lu en partie seulement par l'audit — T1b doit le relire intégralement dans `install.sh` 0.3.2, section `PY_NIGHT`, avant d'agir). Ces correctifs sont signalés « hardware-tested sur 3215672.2 / B26101 » par le flasheur (pas sur D26228). Le dépôt actuel contient les valeurs d'avant correctifs (`ae.c:85 exp_max = 1039`, `rtsp.c:554 O_NONBLOCK`, `night.c:48-49` 1800/800). Les patchs du flasheur sont écrits pour être idempotents (« already present ») : une fois intégrés, le flasheur les saute. Chaque correctif = un commit séparé `fix(ak_rtsp): ...` ; vérification : le flasheur simulé (T1 §7) passe sans erreur ; build OK ; diff minimal. **Branche** `fix/akrtsp-flasher-patches`.

---

## 6. Interactions flasheur ↔ Camyn à connaître (résultat de l'audit)

- Le flasheur est piloté par `install.sh` (1374 lignes, bash) ; les constantes `FIRMWARE_REPO`, `FIRMWARE_COMMIT` sont **codées en dur** (non surchargeables par variable) ; seules `LSC_WIZARD_REPO` / `LSC_WIZARD_BRANCH` (dépôt des scripts d'aide `camera/*.sh`) sont paramétrables. Il n'existe donc pas de chemin « officiel » vers notre fork : voir O-2.
- Étape 1 (SD toolkit) : écrit `wifi.conf` dans la SD, force `TELNET=1`, `TELNET_PORT=24`, ajoute un hook direct dans `hack.sh`, fait sauvegarder les 8 partitions (vérif de tailles et MD5), enregistre le Wi-Fi dans `/etc/config/wpa_supplicant.conf` (cohérent avec notre `ak_rtsp_wrapper.sh` qui lit ce même fichier).
- Étape 2 : construit (§4), copie l'image + `flash_tool` sur la SD, se connecte en telnet:24, lance `decloud_flash.sh` (vérifie la table MTD exacte, la taille de l'image, que l'APP courant == sauvegarde, flashe, **restaure automatiquement le stock en cas d'échec**), puis vérifie 554 et affiche 8091. Le contrôle post-flash ne couvre pas SSH.
- Étiquettes acceptées : 3215672.2 B26101 (testé), 3215672.2 C26101 et 3215672.3 D26228 (« rapportés » par un utilisateur, non testés par l'auteur).

---

## 7. Décisions ouvertes pour l'utilisateur

**O-1. Faut-il intégrer au source d'`ak_rtsp` les 4 correctifs que le flasheur applique à la volée ?** (Contradiction apparente avec « `ak_rtsp` inchangé fonctionnellement ».)
- (a) Non : source = HEAD actuel ; si on utilise le flasheur, il applique ses patchs ; si on construit seul, on obtient la version SANS correctifs (flux H.264 potentiellement corrompu en TCP, exposition à régler à la main, bascule jour/nuit moins fiable).
- (b) Oui, via T1b, après relecture du 4e patch : le build autonome et le build via flasheur donnent le même comportement testé par l'amont.
- **Recommandation : (b)**, en commits séparés, car le critère « H.264 1080p » du brief dépend du correctif RTSP/TCP et que ces correctifs sont les seuls ayant un retour matériel. Si vous préférez (a), le build autonome doit afficher un avertissement.

**O-2. Comment utiliser le flasheur 0.3.2 avec notre fork ?**
- (a) Un fork GitHub du flasheur où l'on change `FIRMWARE_REPO`/`FIRMWARE_COMMIT` (compte GitHub et commit poussé requis ; maintenance du fork).
- (b) Script dans notre dépôt (`tools/flasher/run_with_camyn.sh`, tâche à ajouter après vote) qui télécharge `install.sh` d'un commit figé du flasheur, vérifie son sha256, remplace les 2 constantes par `sed` (et éventuellement l'URL du dépôt), puis l'exécute ; pas de fork nécessaire mais exige que notre commit soit cloneable (poussé).
- (c) Ne pas utiliser le flasheur pour notre image : suivre uniquement notre procédure (backup manuel, `build_firmware.sh`, `install_with_flash_tool.sh`), le flasheur n'étant alors qu'une référence.
- **Recommandation : (c) pour la première caméra** (le flasheur n'a pas été conçu pour des patchs additionnels et son Telnet 24 sans mot de passe est un risque), puis (b) une fois Camyn validé sur matériel.

**O-3. Types de clés autorisées pour se connecter.** (a) ed25519 uniquement (binaire plus petit, moins de surface) ; (b) ed25519 + RSA (si votre client n'a qu'une clé RSA). **Recommandation : (a)** ; vous générez une clé ed25519 dédiée (`ssh-keygen -t ed25519`).

**O-4. Si l'audit du vrai dump montre que telnetd/ftpd sont lancés depuis ROOTFS (hors APP).** (a) accepter la fenêtre d'exposition résiduelle (filet `camyn_lockdown.sh` seul) ; (b) autoriser une modification de ROOTFS (refusée par le brief sans votre validation). **Recommandation : (a) pour V1**, mesurer la fenêtre avec `camyn_hwcheck.sh --boot-window`, ne décider (b) que si la fenêtre est longue ; décision différée à la réception du dump.

**O-5. Embarquer une clé publique par défaut dans l'image ?** (a) Oui : `CAMYN_AUTHORIZED_KEYS_DEFAULT` copiée dans `/usr/local/camyn/authorized_keys.default`, restaurée dans CONFIG au premier boot si absente (survit à un effacement de CONFIG ou à un reset usine) ; l'image devient personnelle (ne pas la publier). (b) Non : seule la microSD fournit les clés (si CONFIG est effacée sans SD à portée, plus d'accès SSH, repli UART). **Recommandation : (a)**, image jamais publiée.

**O-6 (information, pas de choix requis immédiat).** Clés d'hôte régénérées après effacement de CONFIG : le client verra un changement d'empreinte. Rien à décider ; documenté.

---

## 8. Faits vérifiés vs hypothèses

### Vérifié dans les sources (lecture de code/dépôts)

| Fait | Source |
|---|---|
| Dropbear `DROPBEAR_2026.94` existe : release 2026-07-23, tag léger → commit `28216cd9…` ; releases précédentes 2026.93 (2026-07-21), .92, .91 | `gh api repos/mkj/dropbear` |
| Option `-D <dir>` (répertoire d'`authorized_keys`, défaut `~/.ssh`), `-r keyfile`, `-R` (génère les clés d'hôte à la demande, nécessite `DROPBEAR_DELAY_HOSTKEY`), `-F`, `-E`, `-w`, `-T` existent ; `-s/-g/-B` n'existent que si l'auth par mot de passe est compilée | `src/svr-runopts.c` 2026.94 |
| Dropbear refuse un `authorized_keys` dont un composant du chemin n'est pas à root/utilisateur ou est inscriptible par groupe/autres ; vérification jusqu'au home ou `/` | `src/svr-authpubkey.c` `checkpubkeyperms` |
| Dropbear vérifie que le shell de l'utilisateur figure dans `/etc/shells` (sinon `getusershell()` retombe sur `/bin/sh`,`/bin/csh`) ; shell vide = `/bin/sh` | `src/svr-auth.c` |
| Binaire multi-appel : `make PROGRAMS="dropbear dropbearkey scp" MULTI=1 [SCPPROGRESS=1]` puis liens symboliques ; `PROGRAMS=dropbear` retire le code client ; `--disable-zlib`, `-Wl,--gc-sections`, musl recommandés pour la taille | `MULTI.md`, `SMALL.md`, `Makefile.in` |
| Options de `localoptions.h` (noms exacts) : voir T2 | `src/default_options.h` |
| Défauts : X11 déjà 0 ; AGENTFWD, RSA, ECDSA, ED25519, PASSWORD_AUTH à 1 ; DSS à 0 | idem |
| `getrandom()` utilisé pour l'entropie (musl l'expose) | `src/dbrandom.c` |
| Zig 0.14.1 x86_64-linux sha256 `24aeeec8…` (conforme à `ziglang.org/download/index.json`) | téléchargement de l'index |
| SmolRTSP : HEAD `8ae3b51…` ; 4 deps et tags : slice99 v0.7.8, metalang99 v1.13.5, datatype99 v1.6.5, interface99 v1.0.2 (CMakeLists) | clone `OpenIPC/smolrtsp` |
| Flasheur 0.3.2 : fonctionnement détaillé en §4 et §6 (constantes codées en dur, patchs ak_rtsp à la volée, contrat `build_firmware.sh`, sauvegarde des 8 partitions avec contrôle de taille/MD5, flash via telnet:24 avec auto-restauration) | `install.sh`, `camera/decloud_*.sh` |
| `telnet.sh` du toolkit : `telnetd -p ${TELNET_PORT} -l /bin/sh` (sans authentification), défaut port 24, boucle de relance ; `ftp.sh` : `tcpsvd 0 ${FTP_PORT}` | `lsc-tuya-toolkit/sd_card/custom/scripts/` |
| Chaîne de boot : `init → rcS → rc.local → /usr/sbin/main.sh → /tmp/service.sh start` ; `service.sh` monte la SD, teste des marqueurs (`_ht_ap_mode.conf` → exécute `hostapd` de la SD → `hack.sh` en root ; `_ht_av_mode.conf` → FTP ; `_ak39_factory.ini` → mode usine) ; `telnetd`/`ftpd` lancés par `system_run_init` de `service.sh` | READMEs toolkit + `Decloud/docs/HARDWARE.md`, `LOGBOOK.md` (modèle voisin) |
| Le mode « factory test » d'`anyka_ipc` ouvre telnet:23 (chemin supprimé avec `anyka_ipc`) | README toolkit |
| APP stock = SquashFS **xz**, blocs 128 Kio, noyau Linux 4.4.302 ; ROOTFS = squashfs ; CONFIG = JFFS2 ; `/etc/passwd` et `/etc/shadow` = liens symboliques vers `/etc/config` ; `/etc/config` monté en rw | `Decloud/docs/HARDWARE.md` (modèle voisin) ; dépôt courant (ROOTFS « gzip », APP « XZ ») |
| Une image APP **gzip** produite par ce dépôt a été flashée avec succès sur matériel par l'amont et par des utilisateurs du flasheur (rapports du flasheur) | flasheur README/`install.sh` |
| Décompression défaillante signalée après écriture `sf erase+write` non fiable (corruption silencieuse) — d'où l'importance du `flash_tool` par blocs de 4 Kio avec relecture | `Decloud/docs/FLASHING.md` |
| Fixture : le `.gitignore` actuel ignore `bin/`, `*.bin`, `*.squashfs` (piège pour les fixtures) | `.gitignore` du dépôt |
| `Makefile` actuel dépend de `pwsh` (version, clean) et `wsl` (firmware) ; `upload` dépend de FTP | `src/ak_rtsp/Makefile` |

### Hypothèses (non confirmables sans la caméra ou son dump)

1. **Code SI D26228** et compatibilité exacte du modèle 3215672.3 : étiquette à lire ; le flasheur la dit « rapportée » seulement.
2. **Origine exacte de telnetd/ftpd sur CETTE caméra** : très probablement `/usr/sbin/service.sh` (APP), par analogie avec HT-IPC280/6.2802 et LSC 6.2863 ; non vérifié sur le firmware réel (le dépôt ne contient pas `service.sh`). Alternatives : `rc.local`/`rcS` (ROOTFS), `ethernet.sh`, scripts Wi-Fi, `anyka_ipc` lui-même (supprimé). Le watchdog stock ne relance a priori que les entrées de `_ht_process.ini` (donc pas telnetd) : **non vérifié**.
3. **Format SquashFS accepté pour l'APP** : gzip prouvé en pratique par l'amont (image flashée et bootée), xz prouvé par le stock ; la doc du dépôt dit « APP = XZ » et le build produit « gzip » : les deux semblent supportés, mais seul un boot réel de NOTRE image le confirme pour cette caméra. (Les `CONFIG_SQUASHFS_XZ/ZLIB` du noyau ne sont pas lisibles sans `/proc/config.gz`.)
4. **Espace libre dans l'APP** après retrait d'`anyka_ipc` (5,9 Mio) et ajout de Dropbear (≈ 0,2–0,4 Mio, estimation) : inconnu tant que le dump n'existe pas.
5. **`$HOME` de root = `/`** (d'après le format de la ligne `passwd` documentée) : sans importance grâce à `-D`.
6. **Point de montage de la microSD** (`/mnt`, `/tmp/sd`) selon le chemin de boot (le toolkit utilise `/tmp/sd`, `tf_mount.sh` du stock monte sur `/mnt`) : à confirmer, d'où les chemins configurables en tête de `camyn_ssh.sh`/`camyn_lockdown.sh`.
7. **Modes/propriétaires de `/etc/config`, `/etc` et `/`** acceptables pour `checkpubkeyperms` (root, non inscriptibles par groupe/autres) ; présence de `/dev/pts` monté (nécessaire au shell SSH interactif) ; `/etc/shells` absent ou contenant `/bin/sh`.
8. **Entropie au premier boot** : `getrandom` peut bloquer un moment sur noyau 4.4 sans RTC ; sans conséquence pour `ak_rtsp` car la génération de clé est en arrière-plan, mais l'accès SSH initial peut arriver tard.
9. **Un reset usine / un effacement de CONFIG** peut effacer `/etc/config/dropbear` (comportement de `recover_cfg.sh` non lu) : atténué par la clé par défaut (O-5).
10. **Compatibilité zig cc / musl ARM926 pour Dropbear** (autoconf avec `zig ar`/`zig ranlib`, `getrandom`, `forkpty`) : probable (le même compilateur construit déjà `ak_rtsp`) mais non essayée ; T2 la tranche à la compilation, pas à l'exécution.
11. **Non-régression du comportement d'`ak_rtsp`** par le seul changement de build : vérifiable par construction (aucun C modifié), pas par exécution.
12. Les **patchs du flasheur** n'ont pas été relus intégralement (le 4e, mutex dans `night.c`) : T1b doit le faire.
13. **Mémoire** : empreinte RAM de Dropbear + shells, effet sur les 64 Mio : à mesurer (`camyn_hwcheck.sh`).
14. **Comportement de la SD du flasheur après installation** (marqueurs `_ht_ap_mode.conf`, `hack.sh`) : suppose que le `service.sh` réel exécute encore le script de la SD ; le patch T4 la neutralise, mais seul le dump réel dit où et comment.

---

## 9. Ordre d'exécution proposé et prérequis utilisateur

1. L'utilisateur installe : `sudo pacman -S --needed squashfs-tools cmake shellcheck file binutils` (aucun agent ne lance sudo).
2. Vague 1 : T1 ∥ T3. Fusion dans `dev` après revue.
3. Vague 2 : T2 ∥ T4.
4. Vague 3 : T5 ∥ T6a.
5. Vague 4 : T6b. En parallèle possible avec la vague 3 uniquement pour les sections qui ne dépendent pas de T5 (à éviter : un seul agent docs, après T5).
6. (optionnel) T1b selon O-1, à placer avant T5 si accepté (T5 n'en dépend pas fonctionnellement).
7. Le dump réel (par l'utilisateur, après sauvegarde des 8 partitions) déclenche : `audit_remote_services.sh` sur l'arbre extrait, relecture de `service.sh`/`ini`/watchdog, décision O-4, puis build réel et premier flash par l'utilisateur.
