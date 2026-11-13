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
    /* { 0x.., ... 32 bytes ... }, */

#endif /* OTA_KEYS_H */
