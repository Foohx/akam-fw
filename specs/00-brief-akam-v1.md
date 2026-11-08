# Akam V1 — brief condensé (source de vérité côté produit)

Version complète fournie par l'utilisateur le 2026-11-07. Ce résumé fait foi pour les agents.

## Objectif
Fork de https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware (ce dépôt). Caméra LSC Smart Connect 1080p réf. 3215672.3 (SI D26228 attendu, à confirmer), SoC Anyka AK3918AV130, ARM926EJ-S/ARMv5TE, 64 Mio RAM, 8 Mio SPI NOR.
Livrer Akam V1 = `ak_rtsp` inchangé fonctionnellement + SSH Dropbear permanent + Telnet/FTP désactivés + build reproductible sous Arch Linux + doc sauvegarde/flash/restauration.

## Règles absolues
- Ne pas réécrire ce qui marche (ak_rtsp, SmolRTSP, vidéo, micro, IR, ISP, Wi-Fi, port 8091 et son protocole LIST/GET/SET).
- On ne reconstruit QUE la partition APP (mtd7, 0x300000–0x800000, 5120 Kio, montée sur /usr). Jamais UBOOT/KERNEL/ROOTFS/DTB en V1. ROOTFS non modifié sans validation explicite de l'utilisateur.
- CONFIG (mtd6, JFFS2 rw, monté sur /etc/config, 256 Kio) = persistance (clés SSH, wpa_supplicant.conf…).
- Aucun flashage, aucune action sur la caméra par les agents. Rien n'est validé sur matériel : ne jamais écrire « validé » pour du code seulement compilé.
- Jamais de dumps d'origine ni de clés privées dans Git ou une image publique. Clés uniques par caméra.
- Licence MIT + crédits conservés ; respecter licences des dépendances (Dropbear = MIT-like).
- Pas d'interface web, API REST, ONVIF, enregistrement, IA, nouveau noyau, abstraction multi-matériel.

## Dropbear
- Source https://github.com/mkj/dropbear, version candidate 2026.94 (vérifier le tag réel ; figer tag + commit + sha256).
- Cross-compilation depuis Arch : `zig cc -target arm-linux-musleabi -mcpu=arm926ej_s`, soft-float, statique de préférence. Vérifier avec `file`/`readelf`.
- Root par clé publique uniquement, aucun mot de passe. Démarrage auto au boot, indépendant de la microSD. Clé d'hôte + authorized_keys persistants dans /etc/config. Faible empreinte RAM/flash (multi-call binaire dropbear+dropbearkey ; options inutiles désactivées : pas de X11, agent forwarding, password auth, etc.).
- Un échec de Dropbear ne doit JAMAIS bloquer ou arrêter ak_rtsp. Cohabiter avec le watchdog stock (`/usr/bin/watchdog`, `_ht_process.ini`) et `ak_rtsp_wrapper.sh`.
- Fichiers de démarrage à étudier : /usr/sbin/main.sh, /usr/sbin/service.sh, _ht_process.ini, ak_rtsp_wrapper.sh.

## Telnet / FTP
- Telnet (23, et 24 maintenance) et FTP (21) : désactivés en fonctionnement normal, pas réactivés par watchdog ni reconnexion réseau. Identifier le mécanisme de lancement exact (ROOTFS ? APP ? scripts intermédiaires).
- Solution d'abord depuis APP ; documenter toute fenêtre d'exposition au boot. Si seule une modif ROOTFS suffit : documenter, ne pas l'implémenter sans validation.
- Garder un accès maintenance pendant les tests jusqu'à SSH validé. Récupération hors réseau : microSD + UART.

## Build
- Arch Linux x86-64. Le build upstream est orienté Windows (gen_version.ps1, WSL) : le rendre natif Linux (bash/make) sans casser l'organisation des fichiers.
- Figer versions des dépendances (SmolRTSP + ses 4 deps, Dropbear, zig).
- build_firmware.sh : partir du dump APP de la caméra (fourni par l'utilisateur, hors Git), appliquer patch ak_rtsp + Dropbear + init SSH + désactivation Telnet, mksquashfs, round-trip, vérif taille avant padding, image de taille exacte mtd7, SHA-256, rapport de place libre.
- Attention : doc dit APP d'origine en SquashFS/XZ, build_firmware.sh produit du gzip. Vérifier ce qu'accepte le noyau avant toute écriture (documenter, la vérification réelle se fera sur la caméra).

## Flasheur
https://github.com/4ddict/Action-Smart-Connect-Flasher v0.3.2 : déterminer comment l'utiliser avec notre fork au lieu de son ak_rtsp upstream. Ne pas supposer qu'il sait intégrer nos modifications.

## Services cibles
RTSP 554 actif ; 8091 actif LAN seulement ; SSH 22 actif ; Telnet 23/24 désactivés ; HTTP 80 aucun service Akam ; FTP 21 désactivé.

## Audio sortant (V1.1, non bloquant)
Référence https://github.com/berobloom/LSC-1080p-Anyka-Decloud (src/aoplay.c) : /dev/pcmC0D0p, ak_pcm, S16LE stéréo 8 kHz, ampli GPIO 69, IOC_SET_SOURCES=6. Pas dans le lot V1.

## Critères d'acceptation V1 (matériel, à cocher par l'utilisateur plus tard)
Réf. matérielle confirmée ; 8 partitions sauvegardées + SHA-256 ; boot sans microSD ; Wi-Fi auto ; H.264 1080p ; micro audible ; nuit OK ; 8091 OK ; Dropbear auto ; SSH root par clé ; persiste après coupure ; Telnet inaccessible et jamais réactivé ; FTP off ; pas de dépendance Internet ; mémoire OK ; image tient dans APP ; restauration documentée.
