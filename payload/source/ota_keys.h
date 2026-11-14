#ifndef OTA_KEYS_H
#define OTA_KEYS_H

/* Ed25519 public keys that sign OTA release manifests (scripts/publish_ota.py, release.yml).
 * The private half lives only in the repository secret OTA_SIGNING_KEY.
 *
 * To create the key pair:  cc -O2 -Ipayload/source -o /tmp/ota_sign tools/ota_sign.c \
 *                              payload/source/tweetnacl.c && /tmp/ota_sign keygen
 * Put the printed public key line below and the printed secret into OTA_SIGNING_KEY.
 * Keep the previous key in the list while Hubs may still meet manifests signed with it.
 *
 * With no key in the list every manifest is refused, so a Hub built that way cannot update
 * over the air (deploy_hub.py still works). A build can add one more key with
 * -DOTA_EXTRA_PUBKEY=0x..,0x.. (32 bytes): tests and local release testing. */

#define OTA_RELEASE_PUBKEYS \
    { 0xde, 0x5a, 0x64, 0xf4, 0x9b, 0x11, 0x99, 0x4e, 0x1b, 0x9b, 0x0e, 0x5a, 0xf3, 0x5f, 0x8c, 0xe9, \
      0xfc, 0xe8, 0x1b, 0x60, 0xdb, 0xa9, 0xaf, 0x55, 0x81, 0x7c, 0x48, 0x0c, 0xf8, 0x15, 0x9f, 0xf0 },

#endif /* OTA_KEYS_H */
