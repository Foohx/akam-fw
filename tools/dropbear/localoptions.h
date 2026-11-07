/*
 * Camyn Dropbear options (copied over src/localoptions.h by build_dropbear.sh).
 * Written against DROPBEAR_2026.94 default_options.h. Goal: public-key-only,
 * ed25519-only server with shell and command execution, nothing else.
 */
#ifndef CAMYN_LOCALOPTIONS_H
#define CAMYN_LOCALOPTIONS_H

/* --- Authentication: public key only ------------------------------------ */
/* No password logins (this also removes the -s -g -B -t options). */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
/* authorized_keys option parsing (command=, from=...) is not needed: the
 * provisioning script only accepts plain "ssh-ed25519 <key>" lines. */
#define DROPBEAR_SVR_PUBKEY_OPTIONS 0
/* Never take a password from the environment. */
#define DROPBEAR_USE_PASSWORD_ENV 0

/* --- Key types: ed25519 only --------------------------------------------- */
#define DROPBEAR_ED25519 1
#define DROPBEAR_RSA 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_SK_KEYS 0

/* --- Key exchange: curve25519-sha256 only -------------------------------- */
/* Supported by every OpenSSH client since 6.5 (2014). The post-quantum hybrids
 * are dropped for size; clients fall back to plain curve25519. */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_ECDH 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_DH_GROUP14_SHA1 0
#define DROPBEAR_DH_GROUP16 0
#define DROPBEAR_DH_GROUP1 0
#define DROPBEAR_DH_GROUP1_CLIENTONLY 0

/* --- Ciphers and MACs ----------------------------------------------------- */
#define DROPBEAR_CHACHA20POLY1305 1
/* AES-CTR kept for clients without chacha20-poly1305. */
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_3DES 0
#define DROPBEAR_ENABLE_CBC_MODE 0
#define DROPBEAR_ENABLE_GCM_MODE 0
#define DROPBEAR_SHA1_HMAC 0
#define DROPBEAR_SHA1_96_HMAC 0
#define DROPBEAR_SHA2_256_HMAC 1
#define DROPBEAR_SHA2_512_HMAC 0

/* --- Forwarding: none ------------------------------------------------------ */
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0

/* --- Misc ------------------------------------------------------------------ */
/* No sftp-server exists on the camera; scp is not shipped either. */
#define DROPBEAR_SFTPSERVER 0
/* No /etc/motd handling. */
#define DO_MOTD 0
#define DROPBEAR_SMALL_CODE 1
/* Keep delayed host key loading so "dropbear -R" works as a fallback. */
#define DROPBEAR_DELAY_HOSTKEY 1

#endif /* CAMYN_LOCALOPTIONS_H */
