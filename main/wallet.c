#ifndef AMALGAMATED_BUILD
#include "wallet.h"
#include "descriptor.h"
#include "jade_assert.h"
#include "jade_wally_verify.h"
#include "keychain.h"
#include "sensitive.h"
#include "signer.h"
#include "utils/malloc_ext.h"
#include "utils/network.h"
#include "utils/util.h"

#include <stdio.h>
#include <string.h>

#include <wally_anti_exfil.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_bip85.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include <wally_elements.h>
#include <wally_script.h>
#include <wally_transaction.h>

#include <mbedtls/base64.h>
#include <sodium/utils.h>

// GA derived key index, and fixed GA key message
static const uint32_t GA_PATH_ROOT = BIP32_INITIAL_HARDENED_CHILD + 0x4741;
static const uint8_t GA_KEY_MSG[] = "GreenAddress.it HD wallet path";

// Restrictions on GA BIP32 path elements
static const uint32_t SUBACT_ROOT = BIP32_INITIAL_HARDENED_CHILD + 3;
static const uint32_t SUBACT_FLOOR = BIP32_INITIAL_HARDENED_CHILD;
static const uint32_t SUBACT_CEILING = BIP32_INITIAL_HARDENED_CHILD + 16384;
static const uint32_t PATH_BRANCH = 1;
static const uint32_t MAX_PATH_PTR = 10000;

static const uint32_t BIP44_COIN_BTC = BIP32_INITIAL_HARDENED_CHILD;
static const uint32_t BIP44_COIN_TEST = BIP32_INITIAL_HARDENED_CHILD + 1;
static const uint32_t BIP44_COIN_LBTC = BIP32_INITIAL_HARDENED_CHILD + 1776;
static const uint32_t BIP44_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 44;
static const uint32_t BIP45_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 45;
static const uint32_t BIP48_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 48;
static const uint32_t BIP49_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 49;
static const uint32_t BIP84_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 84;
static const uint32_t BIP86_PURPOSE = BIP32_INITIAL_HARDENED_CHILD + 86;

// Maximum number of csv blocks allowed in csv scripts
static const uint32_t MAX_CSV_BLOCKS_ALLOWED = 65535;

// multisig script length for n keys (m-of-n)
#define MULTISIG_SCRIPT_LEN(n) (3 + (n * (EC_PUBLIC_KEY_LEN + 1)))

// Supported script variants (as well as the default green multisig/csv)
// 1. singlesig
#define VARIANT_P2PKH "pkh(k)"
#define VARIANT_P2WPKH "wpkh(k)"
#define VARIANT_P2WPKH_P2SH "sh(wpkh(k))"
#define VARIANT_P2TR "tr(k)"
// 2. multisig
#define VARIANT_MULTI_P2WSH "wsh(multi(k))"
#define VARIANT_MULTI_P2SH "sh(multi(k))"
#define VARIANT_MULTI_P2WSH_P2SH "sh(wsh(multi(k)))"

// CSV script len varies depending on varint 'blocks' (between 1 byte and 3 bytes)
#define CSV_MIN_SCRIPT_LEN (9 + (2 * (EC_PUBLIC_KEY_LEN + 1)) + 1 + 1)
#define CSV_MAX_SCRIPT_LEN (9 + (2 * (EC_PUBLIC_KEY_LEN + 1)) + 1 + 3)

// BTC (rather, non-liquid) uses an optimised, mini-script compatible csv script which is slightly shorter
#define CSV_MIN_SCRIPT_LEN_OPT (6 + (2 * (EC_PUBLIC_KEY_LEN + 1)) + 1 + 1)
#define CSV_MAX_SCRIPT_LEN_OPT (6 + (2 * (EC_PUBLIC_KEY_LEN + 1)) + 1 + 3)

static const char MAINNET_SERVICE_XPUB[]
    = "xpub661MyMwAqRbcGsMS1UQfLrVW52iFHhKd1WbL4BVBZt8xz8pE6oyz6La2LscN2WADtpZZXwKo4DXMbzUdxVsLYxm7f6instfCnpB3cFdbi2F";
static const char TESTNET_SERVICE_XPUB[]
    = "tpubD6NzVbkrYhZ4Y9k7T65kw2Sx9z67CzZr2Hi7w2pkKutUvm25ryvL79PqQTtDvAaYacd4z5NQTMmdJ37t8VbMVZbDY1z2rqUKLRNpVW6rGC3";
static const char LIQUID_SERVICE_XPUB[]
    = "xpub661MyMwAqRbcEZr3uYPEEP4X2bRmYXmxrcLMH8YEwLAFxonVGqstpNywBvwkUDCEZA1cd6fsLgKvb6iZP5yUtLc3G3L8WynChNJznHLaVrA";
static const char TESTNETLIQUID_SERVICE_XPUB[]
    = "tpubD6NzVbkrYhZ4YKB74cMgKEpwByD7UWLXt2MxRdwwaQtgrw6E3YPQgSRkaxMWnpDXKtX5LvRmY5mT8FkzCtJcEQ1YhN1o8CU2S5gy9TDFc24";

struct ext_key MAINNET_SERVICE;
struct ext_key TESTNET_SERVICE;
struct ext_key LIQUID_SERVICE;
struct ext_key TESTNETLIQUID_SERVICE;

// network id to relevant GA service key
static struct ext_key* network_to_ga_service(const network_t network_id)
{
    switch (network_id) {
    case NETWORK_BITCOIN:
        return &MAINNET_SERVICE;
    case NETWORK_LIQUID:
        return &LIQUID_SERVICE;
    case NETWORK_BITCOIN_TESTNET:
    case NETWORK_BITCOIN_REGTEST:
    case NETWORK_LIQUID_REGTEST: // localtest-liquid uses the regtest/testnet key
        return &TESTNET_SERVICE;
    case NETWORK_LIQUID_TESTNET:
        return &TESTNETLIQUID_SERVICE;
    case NETWORK_NONE:
        break;
    }
    JADE_LOGE("Unknown network: %d", network_id);
    return NULL;
}

void wallet_init(void)
{
    JADE_WALLY_VERIFY(bip32_key_from_base58(MAINNET_SERVICE_XPUB, &MAINNET_SERVICE));
    JADE_WALLY_VERIFY(bip32_key_from_base58(TESTNET_SERVICE_XPUB, &TESTNET_SERVICE));
    JADE_WALLY_VERIFY(bip32_key_from_base58(LIQUID_SERVICE_XPUB, &LIQUID_SERVICE));
    JADE_WALLY_VERIFY(bip32_key_from_base58(TESTNETLIQUID_SERVICE_XPUB, &TESTNETLIQUID_SERVICE));
}

// Outputs eg. "m/a'/b'/c/d" - ie. uses m/ as master, and ' as hardened indicator
bool wallet_bip32_path_as_str(const uint32_t* parts, const size_t num_parts, char* output, const size_t output_len)
{
    JADE_ASSERT(parts);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len > 16);

    output[0] = 'm';
    output[1] = '\0';

    for (size_t pos = 1, i = 0; i < num_parts; ++i) {
        uint32_t val = parts[i];
        const char* fmt = "/%u";

        if (ishardened(val)) {
            val = unharden(val);
            fmt = "/%u'"; // hardened
        }

        const size_t freespace = output_len - pos;
        const int nchars = snprintf(output + pos, freespace, fmt, val);
        if (nchars < 0 || nchars > freespace) {
            return false;
        }
        pos += nchars;
    }
    return true;
}

// Accepts "m/a/b/c/d" - accepts m/ or M/ as master, and h, H or ' as hardened indicators
bool wallet_bip32_path_from_str(
    const char* pathstr, const size_t str_len, uint32_t* path, const size_t path_len, size_t* written)
{
    JADE_ASSERT(pathstr);
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);
    JADE_INIT_OUT_SIZE(written);

    // Defer to wally impl
    if (bip32_path_from_str_n(pathstr, str_len, 0, 0, BIP32_FLAG_ALLOW_UPPER, path, path_len, written) != WALLY_OK) {
        JADE_LOGE("Error parsing path string: %.*s", str_len, pathstr);
        return false;
    }
    if (*written > path_len) {
        JADE_LOGE("bip32 path too long (max length: %u): %.*s", path_len, str_len, pathstr);
        return false;
    }
    return true;
}

// Get expected length of script for passed variant
size_t script_length_for_variant(const script_variant_t variant)
{
    switch (variant) {
    case P2PKH:
        return WALLY_SCRIPTPUBKEY_P2PKH_LEN;
    case P2WPKH:
        return WALLY_SCRIPTPUBKEY_P2WPKH_LEN;
    case P2TR:
        return WALLY_SCRIPTPUBKEY_P2TR_LEN;
    case GREEN:
    case P2WPKH_P2SH:
    case MULTI_P2SH:
    case MULTI_P2WSH_P2SH:
        return WALLY_SCRIPTPUBKEY_P2SH_LEN;
    case MULTI_P2WSH:
        return WALLY_SCRIPTPUBKEY_P2WSH_LEN;
    default:
        return 0;
    }
}

// Map a script-variant enum value into the corresponding string
const char* get_script_variant_string(const script_variant_t variant)
{
    switch (variant) {
    case P2PKH:
        return VARIANT_P2PKH;
    case P2WPKH:
        return VARIANT_P2WPKH;
    case P2WPKH_P2SH:
        return VARIANT_P2WPKH_P2SH;
    case P2TR:
        return VARIANT_P2TR;
    case MULTI_P2WSH:
        return VARIANT_MULTI_P2WSH;
    case MULTI_P2SH:
        return VARIANT_MULTI_P2SH;
    case MULTI_P2WSH_P2SH:
        return VARIANT_MULTI_P2WSH_P2SH;
    default:
        return NULL;
    }
}

// Map a script-variant enum value into its BIP-44 'purpose' value
static uint32_t get_script_variant_purpose(const script_variant_t variant)
{
    switch (variant) {
    case P2PKH:
        return BIP44_PURPOSE;
    case P2WPKH:
        return BIP84_PURPOSE;
    case P2WPKH_P2SH:
        return BIP49_PURPOSE;
    case P2TR:
        return BIP86_PURPOSE;
    case MULTI_P2SH:
        return BIP45_PURPOSE;
    case MULTI_P2WSH:
    case MULTI_P2WSH_P2SH:
        return BIP48_PURPOSE;
    default:
        JADE_ASSERT(false); // Green or unhandled case
    }
}

// Map a script-variant string into the corresponding enum value
bool get_script_variant(const char* variant, const size_t variant_len, script_variant_t* output)
{
    JADE_ASSERT(output);

    // Default to Green multisig/csv
    if (variant == NULL || variant_len == 0) {
        *output = GREEN;
        // Singlesig
    } else if (strcmp(VARIANT_P2PKH, variant) == 0) {
        *output = P2PKH;
    } else if (strcmp(VARIANT_P2WPKH, variant) == 0) {
        *output = P2WPKH;
    } else if (strcmp(VARIANT_P2WPKH_P2SH, variant) == 0) {
        *output = P2WPKH_P2SH;
    } else if (strcmp(VARIANT_P2TR, variant) == 0) {
        *output = P2TR;
        // Multisig
    } else if (strcmp(VARIANT_MULTI_P2SH, variant) == 0) {
        *output = MULTI_P2SH;
    } else if (strcmp(VARIANT_MULTI_P2WSH, variant) == 0) {
        *output = MULTI_P2WSH;
    } else if (strcmp(VARIANT_MULTI_P2WSH_P2SH, variant) == 0) {
        *output = MULTI_P2WSH_P2SH;
    } else {
        // Unrecognised
        JADE_LOGW("Unrecognised script variant: %s", variant);
        return false;
    }
    return true;
}

// Assumes p2sh script is wrapping p2wpkh.
// multisig not handled atm.
bool get_singlesig_variant_from_script_type(const size_t script_type, script_variant_t* variant)
{
    JADE_ASSERT(variant);

    switch (script_type) {
    case WALLY_SCRIPT_TYPE_P2WPKH:
        *variant = P2WPKH;
        return true;
    case WALLY_SCRIPT_TYPE_P2PKH:
        *variant = P2PKH;
        return true;
    case WALLY_SCRIPT_TYPE_P2SH:
        *variant = P2WPKH_P2SH; // assumed
        return true;
    case WALLY_SCRIPT_TYPE_P2TR:
        *variant = P2TR;
        return true;
    default:
        return false;
    }
}

bool is_greenaddress(const script_variant_t variant) { return variant == GREEN; }

bool is_singlesig(const script_variant_t variant)
{
    return variant == P2PKH || variant == P2WPKH || variant == P2WPKH_P2SH || variant == P2TR;
}

bool is_multisig(const script_variant_t variant)
{
    return variant == MULTI_P2SH || variant == MULTI_P2WSH || variant == MULTI_P2WSH_P2SH;
}

// Function to apply a path to a serialised key to derive a leaf key
bool wallet_derive_pubkey(const uint8_t* serialised_key, const size_t key_len, const uint32_t* path,
    const size_t path_len, uint32_t flags, struct ext_key* hdkey)
{
    if (!serialised_key || key_len != BIP32_SERIALIZED_LEN || (!path && path_len != 0) || !hdkey) {
        return false;
    }

    if (path && path_len > 0) {
        // De-serialise the key into a temporary
        struct ext_key root;
        SENSITIVE_PUSH(&root, sizeof(root));
        if (bip32_key_unserialize(serialised_key, key_len, &root) != WALLY_OK) {
            JADE_LOGE("Failed to de-serialise key");
            SENSITIVE_POP(&root);
            return false;
        }

        // Apply any additional path
        flags |= BIP32_FLAG_KEY_PUBLIC;
        JADE_WALLY_VERIFY(bip32_key_from_parent_path(&root, path, path_len, flags, hdkey));
        SENSITIVE_POP(&root);
    } else {
        // De-serialise the key directly into the output
        if (bip32_key_unserialize(serialised_key, key_len, hdkey) != WALLY_OK) {
            JADE_LOGE("Failed to de-serialise key");
            return false;
        }
    }
    return true;
}

// Function to apply a path to an xpub to derive a leaf key
bool wallet_derive_from_xpub(
    const char* xpub, const uint32_t* path, const size_t path_len, const uint32_t flags, struct ext_key* hdkey)
{
    if (!xpub) {
        return false;
    }

    size_t written = 0;
    uint8_t bytes[BIP32_SERIALIZED_LEN + BASE58_CHECKSUM_LEN];
    SENSITIVE_PUSH(bytes, sizeof(bytes));
    if (wally_base58_to_bytes(xpub, BASE58_FLAG_CHECKSUM, bytes, sizeof(bytes), &written) != WALLY_OK
        || written != BIP32_SERIALIZED_LEN) {
        JADE_LOGE("Failed to parse xpub: '%s'", xpub);
        SENSITIVE_POP(bytes);
        return false;
    }
    const bool ret = wallet_derive_pubkey(bytes, written, path, path_len, flags, hdkey);
    SENSITIVE_POP(bytes);
    return ret;
}

// Function to get the path used when we are asked to export an xpub
void wallet_get_default_xpub_export_path(
    const script_variant_t variant, const uint16_t account, uint32_t* path, const size_t path_len, size_t* written)
{
    JADE_ASSERT(!is_greenaddress(variant));
    JADE_ASSERT(path);
    JADE_ASSERT(path_len > 3);
    JADE_INIT_OUT_SIZE(written);

    // 'Purpose' depends on script variant unless bip48 multisig
    path[0] = get_script_variant_purpose(variant);

    if (variant == MULTI_P2SH) {
        // Special case for sh(multi()) - use bip45 (m/45' only)
        *written = 1;
        return;
    }

    // 'Coin-type' depends on network
    // FIXME: Handle liquid
    path[1] = keychain_get_network_type_restriction() == NETWORK_TYPE_TEST ? BIP44_COIN_TEST : BIP44_COIN_BTC;

    // 'Account' is as passed in
    path[2] = harden(account);
    *written = 3;

    if (is_multisig(variant)) {
        // bip48 script type flag
        const uint8_t bip48_script_type = variant == MULTI_P2WSH ? 2 : variant == MULTI_P2WSH_P2SH ? 1 : 0;
        path[3] = harden(bip48_script_type);
        *written += 1;
    }
}

// Internal helper to get a derived private key - note 'output' should point to a buffer of size EC_PRIVATE_KEY_LEN
static void wallet_get_privkey(const uint32_t* path, const size_t path_len, uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(keychain_get());
    JADE_ASSERT(path);
    JADE_ASSERT(path_len > 0);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == EC_PRIVATE_KEY_LEN);

    JADE_LOGD("path_len %d", path_len);

    struct ext_key derived;
    SENSITIVE_PUSH(&derived, sizeof(derived));
    JADE_WALLY_VERIFY(bip32_key_from_parent_path(
        &keychain_get()->xpriv, path, path_len, BIP32_FLAG_KEY_PRIVATE | BIP32_FLAG_SKIP_HASH, &derived));

    memcpy(output, derived.priv_key + 1, output_len);
    SENSITIVE_POP(&derived);
}

static uint32_t get_bip44_coin_type(const network_t network_id)
{
    switch (network_id) {
    case NETWORK_BITCOIN:
        return BIP44_COIN_BTC;
    case NETWORK_LIQUID:
        return BIP44_COIN_LBTC;
    case NETWORK_BITCOIN_TESTNET:
    case NETWORK_LIQUID_TESTNET:
    case NETWORK_BITCOIN_REGTEST:
    case NETWORK_LIQUID_REGTEST:
        return BIP44_COIN_TEST;
    case NETWORK_NONE:
        break;
    }
    JADE_ASSERT(false); // Invalid network
}

bool wallet_is_expected_singlesig_path(const network_t network_id, const script_variant_t script_variant,
    const bool is_change, const uint32_t* path, const size_t path_len)
{
    JADE_ASSERT(is_singlesig(script_variant));
    JADE_ASSERT(path);

    // Check path is bip44-like (bip49, bip84 etc.)
    if (path_len != 5) {
        return false;
    }

    if (path[0] != get_script_variant_purpose(script_variant)) {
        return false;
    }

    const uint32_t expected_coin_type = get_bip44_coin_type(network_id);
    if (path[1] != expected_coin_type) {
        return false;
    }

    if (path[2] < SUBACT_FLOOR || path[2] >= SUBACT_CEILING) {
        return false;
    }

    if (path[3] != (is_change ? 1 : 0)) {
        return false;
    }

    if (path[4] >= MAX_PATH_PTR) {
        return false;
    }

    // Looks good
    return true;
}

bool wallet_is_expected_multisig_path(
    const size_t cosigner_index, const bool is_change, const uint32_t* path, size_t path_len)
{
    JADE_ASSERT(path);

    // Check path is at most three unhardened elements: cosigner index, change flag, and index ptr (in range)
    // This covers bip 45/48/87
    // (Path prefix is verified as part of multisig descriptor)
    if (path_len == 0 || path_len > 3) {
        return false;
    }

    if (path_len > 0) {
        if (path[path_len - 1] >= MAX_PATH_PTR) {
            return false;
        }
    }

    if (path_len > 1) {
        if (path[path_len - 2] != (is_change ? 1 : 0)) {
            return false;
        }
    }

    if (path_len > 2) {
        if (path[path_len - 3] != cosigner_index) {
            return false;
        }
    }

    // Looks good
    return true;
}

// Build a valid/expected green address path from the subact, branch and ptr provided
void wallet_build_receive_path(const uint32_t subaccount, const uint32_t branch, const uint32_t pointer,
    uint32_t* output_path, const size_t output_len, size_t* written)
{
    JADE_ASSERT(output_path);
    JADE_ASSERT(output_len >= 4);
    JADE_ASSERT(written);

    if (subaccount > 0) {
        output_path[0] = SUBACT_ROOT;
        output_path[1] = harden(subaccount);
        output_path[2] = branch;
        output_path[3] = pointer;
        *written = 4;
    } else {
        output_path[0] = branch;
        output_path[1] = pointer;
        *written = 2;
    }
}

bool wallet_serialize_gaservice_path(
    uint8_t* serialized, const size_t serialized_len, const uint32_t* gaservice_path, const size_t gaservice_path_len)
{
    if (!serialized || serialized_len != HMAC_SHA512_LEN || !gaservice_path
        || gaservice_path_len != GASERVICE_PATH_LEN) {
        return false;
    }

    for (size_t i = 0; i < gaservice_path_len; ++i) {
        JADE_ASSERT(!(gaservice_path[i] & 0xFFFF0000));
        serialized[2 * i] = gaservice_path[i] >> 8;
        serialized[2 * i + 1] = gaservice_path[i] & 0xFF;
    }
    return true;
}

bool wallet_unserialize_gaservice_path(
    const uint8_t* serialized, const size_t serialized_len, uint32_t* gaservice_path, const size_t gaservice_path_len)
{
    if (!serialized || serialized_len != HMAC_SHA512_LEN || !gaservice_path
        || gaservice_path_len != GASERVICE_PATH_LEN) {
        return false;
    }

    for (size_t i = 0; i < gaservice_path_len; ++i) {
        gaservice_path[i] = (serialized[2 * i] << 8) + serialized[2 * i + 1];
    }
    return true;
}

// Helper to create the service/gait path.
// (The below is correct for newly created wallets, verified in regtest).
bool wallet_calculate_gaservice_path(
    struct ext_key* root_key, uint32_t* gaservice_path, const size_t gaservice_path_len)
{
    if (!root_key || !gaservice_path || gaservice_path_len != (HMAC_SHA512_LEN / 2)) {
        return false;
    }

    // 1. Derive a child of the private key using the fixed GA index
    struct ext_key derived;
    SENSITIVE_PUSH(&derived, sizeof(derived));
    JADE_WALLY_VERIFY(bip32_key_from_parent_path(
        root_key, &GA_PATH_ROOT, 1, BIP32_FLAG_KEY_PRIVATE | BIP32_FLAG_SKIP_HASH, &derived));

    // 2. Get it as an 'extended public key' byte-array
    uint8_t extkeydata[sizeof(derived.chain_code) + sizeof(derived.pub_key)];
    SENSITIVE_PUSH(extkeydata, sizeof(extkeydata));
    memcpy(extkeydata, derived.chain_code, sizeof(derived.chain_code));
    memcpy(extkeydata + sizeof(derived.chain_code), derived.pub_key, sizeof(derived.pub_key));

    // 3. HMAC the fixed GA key message with 2. to yield the 64 'service path' bytes for this mnemonic/private key
    uint8_t serialized[HMAC_SHA512_LEN];
    JADE_WALLY_VERIFY(wally_hmac_sha512(
        GA_KEY_MSG, sizeof(GA_KEY_MSG), extkeydata, sizeof(extkeydata), serialized, sizeof(serialized)));
    SENSITIVE_POP(extkeydata);
    SENSITIVE_POP(&derived);

    // 4. Deserialize to 32 '16 bit' (ie. unhardened) path elements
    return wallet_unserialize_gaservice_path(serialized, sizeof(serialized), gaservice_path, gaservice_path_len);
}

bool wallet_get_gaservice_fingerprint(const network_t network_id, uint8_t* output, size_t output_len)
{
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == BIP32_KEY_FINGERPRINT_LEN);

    const struct ext_key* const service = network_to_ga_service(network_id);
    if (!service) {
        return false;
    }

    // Fingerprint is first 4 bytes of the hash160, which should be populated already
    memcpy(output, service->hash160, output_len);
    return true;
}

static void wallet_get_gaservice_path_root(const bool subaccount_root, uint32_t* ga_path, const size_t ga_path_len)
{
    JADE_ASSERT(ga_path);
    JADE_ASSERT(ga_path_len == GASERVICE_ROOT_PATH_LEN);

    const keychain_t* const keychain = keychain_get();
    JADE_ASSERT(keychain);

    // The first element is 3 for subaccount paths, or 1 otherwise
    ga_path[0] = subaccount_root ? 3 : 1;

    // GA service path goes in elements 1 - 32 incl.
    JADE_STATIC_ASSERT(sizeof(keychain->gaservice_path) == (ga_path_len - 1) * sizeof(ga_path[0]));
    memcpy(&ga_path[1], keychain->gaservice_path, sizeof(keychain->gaservice_path));
}

static bool wallet_get_gaservice_path_tail(const uint32_t* path, const size_t path_len, uint32_t* ga_path,
    const size_t ga_path_len, size_t* written, bool* is_subaccount_path)
{
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);
    JADE_ASSERT(ga_path);
    JADE_ASSERT(ga_path_len >= MAX_GASERVICE_PATH_TAIL_LEN);
    JADE_ASSERT(written);
    JADE_ASSERT(is_subaccount_path);

    // We only support the following cases - populate the path tail
    if (path_len == 2 && path[0] == PATH_BRANCH && path[1] < MAX_PATH_PTR) {
        // 1.  1/ptr
        // service path tail: ptr
        ga_path[0] = path[1];
        *written = 1;
        *is_subaccount_path = false;
        return true;
    }

    if (path_len == 4 && path[0] == SUBACT_ROOT && path[1] > SUBACT_FLOOR && path[1] < SUBACT_CEILING
        && path[2] == PATH_BRANCH && path[3] < MAX_PATH_PTR) {
        // 2.  3'/subact'/1/ptr
        // service path tail: subact/ptr
        ga_path[0] = unharden(path[1]);
        ga_path[1] = path[3];
        *written = 2;
        *is_subaccount_path = true;
        return true;
    }

    // Path pattern not recognised
    return false;
}

// Fetch the wallet's relevant gait service path based on the user signer's path
bool wallet_get_gaservice_path(
    const uint32_t* path, const size_t path_len, uint32_t* ga_path, const size_t ga_path_len, size_t* written)
{
    if (!path || path_len == 0 || !ga_path || ga_path_len != MAX_GASERVICE_PATH_LEN || !written) {
        return false;
    }

    // Populate the path tail based on the user path
    size_t tail_len = 0;
    bool is_subaccount_path = false;
    if (!wallet_get_gaservice_path_tail(path, path_len, &ga_path[GASERVICE_ROOT_PATH_LEN],
            ga_path_len - GASERVICE_ROOT_PATH_LEN, &tail_len, &is_subaccount_path)) {
        // Path pattern not recognised
        return false;
    }

    // Populate GA service path root
    wallet_get_gaservice_path_root(is_subaccount_path, ga_path, GASERVICE_ROOT_PATH_LEN);
    *written = GASERVICE_ROOT_PATH_LEN + tail_len;

    return true;
}

bool wallet_get_gaservice_root_key(
    const struct ext_key* const service, const bool subaccount_root, struct ext_key* gakey)
{
    if (!service || !gakey) {
        return false;
    }

    uint32_t root_path[GASERVICE_ROOT_PATH_LEN];
    wallet_get_gaservice_path_root(subaccount_root, root_path, GASERVICE_ROOT_PATH_LEN);

    JADE_WALLY_VERIFY(
        bip32_key_from_parent_path(service, root_path, GASERVICE_ROOT_PATH_LEN, BIP32_FLAG_KEY_PUBLIC, gakey));

    return true;
}

// Helper to validate the user-path, and fetch the wallet's relevant gait service pubkey
static bool wallet_get_gaservice_key(
    const network_t network_id, const uint32_t* path, const size_t path_len, struct ext_key* gakey)
{
    JADE_ASSERT(path);
    JADE_ASSERT(path_len);
    JADE_ASSERT(gakey);

    const keychain_t* const keychain = keychain_get();
    JADE_ASSERT(keychain);

    const struct ext_key* const service = network_to_ga_service(network_id);
    if (!service) {
        return false;
    }

    uint32_t ga_path_tail[MAX_GASERVICE_PATH_TAIL_LEN];
    size_t ga_path_tail_len = 0;
    bool is_subaccount_path = false;
    if (!wallet_get_gaservice_path_tail(
            path, path_len, ga_path_tail, MAX_GASERVICE_PATH_TAIL_LEN, &ga_path_tail_len, &is_subaccount_path)) {
        // Path pattern not recognised
        return false;
    }

    const struct ext_key* const cached_service_root = keychain_cached_service(service, is_subaccount_path);
    JADE_WALLY_VERIFY(bip32_key_from_parent_path(
        cached_service_root, ga_path_tail, ga_path_tail_len, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, gakey));

    return true;
}

// Helper to wrap a given script or pubkey in p2wsh/p2wpkh (redeem) and p2sh scripts - note 'output' should point to a
// buffer at least WALLY_SCRIPTPUBKEY_P2SH_LEN in length. bytes can be either a pubkey (p2wpkh) or a script (p2wsh) and
// flags should then be either WALLY_SCRIPT_HASH160 (for p2wpkh) or WALLY_SCRIPT_SHA256 (for p2wsh)
static void wallet_p2sh_p2wsh_scriptpubkey_for_bytes(const uint8_t* bytes, const size_t bytes_len, uint32_t flags,
    uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(bytes);
    JADE_ASSERT(bytes_len > 0);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= WALLY_SCRIPTPUBKEY_P2SH_LEN);
    JADE_ASSERT(flags == WALLY_SCRIPT_SHA256 || flags == WALLY_SCRIPT_HASH160);
    JADE_ASSERT(written);

    uint8_t redeem_script[WALLY_SCRIPTPUBKEY_P2WSH_LEN]; // Sufficient for p2wsh and p2wpkh

    // 1. Get redeem script for the passed script
    JADE_WALLY_VERIFY(
        wally_witness_program_from_bytes(bytes, bytes_len, flags, redeem_script, sizeof(redeem_script), written));
    const size_t expected_written
        = flags == WALLY_SCRIPT_SHA256 ? WALLY_SCRIPTPUBKEY_P2WSH_LEN : WALLY_SCRIPTPUBKEY_P2WPKH_LEN;
    JADE_ASSERT(*written == expected_written);

    // 2. Get p2sh script for the redeem script
    JADE_WALLY_VERIFY(wally_scriptpubkey_p2sh_from_bytes(
        redeem_script, expected_written, WALLY_SCRIPT_HASH160, output, output_len, written));
    JADE_ASSERT(*written == WALLY_SCRIPTPUBKEY_P2SH_LEN);
}

// Helper to build an M-of-N [sorted-] multisig script
static void wallet_build_multisig(const bool sorted, const size_t threshold, const uint8_t* pubkeys,
    const size_t pubkeys_len, uint8_t* output, const size_t output_len, size_t* written)
{
    const size_t num_pubkeys = pubkeys_len / EC_PUBLIC_KEY_LEN;
    JADE_ASSERT(num_pubkeys * EC_PUBLIC_KEY_LEN == pubkeys_len);

    JADE_ASSERT(threshold > 0);
    JADE_ASSERT(threshold <= num_pubkeys);

    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= MULTISIG_SCRIPT_LEN(num_pubkeys)); // Sufficient
    JADE_ASSERT(written);

    // Create m-of-n multisig script
    const uint32_t flags = sorted ? WALLY_SCRIPT_MULTISIG_SORTED : 0;
    JADE_LOGD("Generating %uof%u %s multisig script", threshold, num_pubkeys, sorted ? "sorted" : "(unsorted)");
    JADE_WALLY_VERIFY(
        wally_scriptpubkey_multisig_from_bytes(pubkeys, pubkeys_len, threshold, flags, output, output_len, written));
    JADE_ASSERT(*written == MULTISIG_SCRIPT_LEN(num_pubkeys));
}

// Helper to build a 2of2 CSV multisig script
static void wallet_build_csv(const network_t network_id, const uint8_t* pubkeys, const size_t pubkeys_len,
    const size_t blocks, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(network_id != NETWORK_NONE);
    JADE_ASSERT(pubkeys_len == 2 * EC_PUBLIC_KEY_LEN); // 2of2 only

    JADE_ASSERT(blocks > 0);
    JADE_ASSERT(blocks <= MAX_CSV_BLOCKS_ALLOWED);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len >= CSV_MAX_SCRIPT_LEN); // Sufficient
    JADE_ASSERT(written);

    // Create 2of2 CSV multisig script (2of3-csv not supported)
    if (network_is_liquid(network_id)) {
        // NOTE: we use the original (un-optimised) csv script for liquid
        JADE_LOGI("Generating liquid csv script");
        JADE_WALLY_VERIFY(wally_scriptpubkey_csv_2of2_then_1_from_bytes(
            pubkeys, pubkeys_len, blocks, 0, output, output_len, written));
        JADE_ASSERT(*written >= CSV_MIN_SCRIPT_LEN && *written <= CSV_MAX_SCRIPT_LEN);
    } else {
        // NOTE: we use the 'new-improved!' optimised, miniscript-compatible csv script for btc
        JADE_LOGI("Generating optimised csv script");
        JADE_WALLY_VERIFY(wally_scriptpubkey_csv_2of2_then_1_from_bytes_opt(
            pubkeys, pubkeys_len, blocks, 0, output, output_len, written));
        JADE_ASSERT(*written >= CSV_MIN_SCRIPT_LEN_OPT && *written <= CSV_MAX_SCRIPT_LEN_OPT);
    }
}

// Function to build a green-address script - 2of2 or 2of3 multisig, or a 2of2 csv
// Note only the pub_key member is required to be valid from passed ext_key structs.
bool wallet_build_ga_script_ex(const network_t network_id, const struct ext_key* user_key,
    const struct ext_key* recovery_hdkey, const size_t csv_blocks, const uint32_t* path, const size_t path_len,
    uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(keychain_get());

    if (csv_blocks > MAX_CSV_BLOCKS_ALLOWED || !path || !path_len || !output
        || output_len < script_length_for_variant(GREEN) || !written) {
        return false;
    }

    // We do not support 2of3-csv (ie. can't have csv blocks AND a recovery pubkey)
    if (csv_blocks && recovery_hdkey) {
        JADE_LOGE("2of3-csv is not supported");
        return false;
    }

    // If csv, ensure above allowed minimum for network
    if (csv_blocks && !network_is_allowable_csv_blocks(network_id, csv_blocks)) {
        JADE_LOGE("csvblocks (%u) too low for network %d", csv_blocks, network_id);
        return false;
    }

    // The multisig or csv script we generate
    size_t script_len = 0;
    uint8_t script[MULTISIG_SCRIPT_LEN(3)]; // The largest script we might generate

    // The GA and user pubkeys
    uint8_t pubkeys[3 * EC_PUBLIC_KEY_LEN]; // In case of 2of3
    const size_t num_pubkeys = recovery_hdkey ? 3 : 2; // 2of3 if recovery-xpub
    uint8_t* next_pubkey = pubkeys;

    // Get the GA-key for the passed path (if valid)
    struct ext_key gakey;
    if (!wallet_get_gaservice_key(network_id, path, path_len, &gakey)) {
        JADE_LOGE("Failed to derive valid ga key for path");
        return false;
    }
    memcpy(next_pubkey, gakey.pub_key, sizeof(gakey.pub_key));
    JADE_STATIC_ASSERT(sizeof(gakey.pub_key) == EC_PUBLIC_KEY_LEN);
    next_pubkey += sizeof(gakey.pub_key);

    if (user_key) {
        // Copy the already-derived user pubkey provided by the caller
        memcpy(next_pubkey, user_key->pub_key, sizeof(user_key->pub_key));
    } else {
        // Derive the user pubkey from the path and copy it
        struct ext_key derived;
        if (!wallet_get_hdkey(path, path_len, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &derived)) {
            return false;
        }
        memcpy(next_pubkey, derived.pub_key, sizeof(derived.pub_key));
    }
    next_pubkey += EC_PUBLIC_KEY_LEN;

    // Add recovery key also, if one passed
    if (recovery_hdkey) {
        JADE_ASSERT(num_pubkeys == 3);
        memcpy(next_pubkey, recovery_hdkey->pub_key, sizeof(recovery_hdkey->pub_key));
    }

    // Get 2of2 or 2of3, csv or multisig script, depending on params
    if (csv_blocks) {
        wallet_build_csv(
            network_id, pubkeys, num_pubkeys * EC_PUBLIC_KEY_LEN, csv_blocks, script, sizeof(script), &script_len);
    } else {
        const bool sorted = false; // GA multisig is not BIP67 sorted - the keys are provided in the expected order
        wallet_build_multisig(sorted, 2, pubkeys, num_pubkeys * EC_PUBLIC_KEY_LEN, script, sizeof(script), &script_len);
    }

    // Get the p2sh/p2wsh script-pubkey for the script we have created
    wallet_p2sh_p2wsh_scriptpubkey_for_bytes(script, script_len, WALLY_SCRIPT_SHA256, output, output_len, written);
    return true;
}

// Function to build a green-address script - 2of2 or 2of3 multisig, or a 2of2 csv
bool wallet_build_ga_script(const network_t network_id, const char* xpubrecovery, const size_t csv_blocks,
    const uint32_t* path, const size_t path_len, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(keychain_get());

    if (csv_blocks > MAX_CSV_BLOCKS_ALLOWED || !path || !path_len || !output
        || output_len < script_length_for_variant(GREEN) || !written) {
        return false;
    }

    // Derive the recovery key, if one passed
    struct ext_key recovery_hdkey;
    struct ext_key* recovery_p = xpubrecovery ? &recovery_hdkey : NULL;
    if (xpubrecovery) {
        // xpub includes branch, so only need to derive the final step (ptr)
        if (!wallet_derive_from_xpub(xpubrecovery, &path[path_len - 1], 1, BIP32_FLAG_SKIP_HASH, &recovery_hdkey)) {
            JADE_LOGE("Error trying to apply recovery key '%s'", xpubrecovery);
            return false;
        }
    }

    return wallet_build_ga_script_ex(
        network_id, NULL, recovery_p, csv_blocks, path, path_len, output, output_len, written);
}

// Function to build a single-sig script:
// - legacy-p2pkh
// - native segwit v0 p2wpkh
// - p2sh-wrapped segwit v0 p2wpkh
// - segwit v1 p2tr (keyspend only)
bool wallet_build_singlesig_script(const network_t network_id, const script_variant_t script_variant,
    const struct ext_key* hdkey, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(keychain_get());

    if (!is_singlesig(script_variant) || !hdkey || !output || output_len < script_length_for_variant(script_variant)
        || !written) {
        return false;
    }

    const uint8_t* pubkey = hdkey->pub_key;
    const size_t pubkey_len = sizeof(hdkey->pub_key);
    if (script_variant == P2WPKH_P2SH) {
        // Get the p2sh/p2wsh script-pubkey for the passed pubkey
        JADE_LOGD("Generating singlesig p2sh_p2wpkh script");
        wallet_p2sh_p2wsh_scriptpubkey_for_bytes(pubkey, pubkey_len, WALLY_SCRIPT_HASH160, output, output_len, written);
    } else if (script_variant == P2WPKH) {
        // Get a redeem script for the passed pubkey
        JADE_LOGD("Generating singlesig p2wpkh script");
        JADE_WALLY_VERIFY(
            wally_witness_program_from_bytes(pubkey, pubkey_len, WALLY_SCRIPT_HASH160, output, output_len, written));
    } else if (script_variant == P2PKH) {
        // Get a legacy p2pkh script-pubkey for the passed pubkey
        JADE_LOGD("Generating singlesig p2pkh script");
        JADE_WALLY_VERIFY(
            wally_scriptpubkey_p2pkh_from_bytes(pubkey, pubkey_len, WALLY_SCRIPT_HASH160, output, output_len, written));
    } else if (script_variant == P2TR) {
        // Get a p2tr script-pubkey for the passed pubkey.
        // Wally will tweak the pubkey for us according to BIP-341,
        // note the tweak for Liquid is different hence using flags below.
        JADE_LOGD("Generating singlesig p2tr script");
        const uint32_t flags = network_is_liquid(network_id) ? EC_FLAG_ELEMENTS : 0;
        JADE_WALLY_VERIFY(wally_scriptpubkey_p2tr_from_bytes(pubkey, pubkey_len, flags, output, output_len, written));
    } else {
        JADE_ASSERT_MSG(false, "Unrecognised script variant: %u", script_variant);
        return false;
    }
    JADE_ASSERT(*written == script_length_for_variant(script_variant));
    return true;
}

bool wallet_search_for_singlesig_script(const network_t network_id, const script_variant_t script_variant,
    const struct ext_key* search_root, size_t* index, const size_t search_depth, const uint8_t* script,
    const size_t script_len)
{
    JADE_ASSERT(keychain_get());

    if (!is_singlesig(script_variant) || !search_root || !index || !search_depth || !script
        || script_len != script_length_for_variant(script_variant)) {
        return false;
    }

    bool found = false;
    struct ext_key derived;
    uint8_t generated[WALLY_SCRIPTPUBKEY_P2WSH_LEN]; // Sufficient
    for (const size_t end = *index + search_depth; *index < end; ++*index) {
        // Try next leaf
        JADE_WALLY_VERIFY(
            bip32_key_from_parent(search_root, *index, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &derived));

        size_t written = 0;
        if (!wallet_build_singlesig_script(
                network_id, script_variant, &derived, generated, sizeof(generated), &written)) {
            JADE_LOGE("Error generating singlesig script");
            return false;
        }

        // See if generated is identical to the script passed in
        if (written == script_len && !memcmp(generated, script, script_len)) {
            found = true;
            break;
        }
    }
    return found;
}

// Function to build a [sorted-]multi-sig script - p2wsh, p2sh, or p2sh-p2wsh wrapped
bool wallet_build_multisig_script(const script_variant_t script_variant, const bool sorted, const uint8_t threshold,
    const uint8_t* pubkeys, const size_t pubkeys_len, uint8_t* output, const size_t output_len, size_t* written)
{
    JADE_ASSERT(keychain_get());

    if (!is_multisig(script_variant) || !threshold || !pubkeys || !pubkeys_len || !output
        || output_len < script_length_for_variant(script_variant) || !written) {
        return false;
    }

    // Build a standard multisig script
    uint8_t multisig_script[MULTISIG_SCRIPT_LEN(MAX_ALLOWED_SIGNERS)]; // Sufficient
    wallet_build_multisig(sorted, threshold, pubkeys, pubkeys_len, multisig_script, sizeof(multisig_script), written);

    // Wrap as appropriate
    if (script_variant == MULTI_P2WSH_P2SH) {
        // Get the p2sh/p2wsh script-pubkey for the passed pubkey
        JADE_LOGD("Generating multisig p2sh_p2wsh script");
        wallet_p2sh_p2wsh_scriptpubkey_for_bytes(
            multisig_script, *written, WALLY_SCRIPT_SHA256, output, output_len, written);
    } else if (script_variant == MULTI_P2WSH) {
        // Get a redeem script for the passed pubkey
        JADE_LOGD("Generating multisig p2wsh script");
        JADE_WALLY_VERIFY(wally_witness_program_from_bytes(
            multisig_script, *written, WALLY_SCRIPT_SHA256, output, output_len, written));
    } else if (script_variant == MULTI_P2SH) {
        // Get a multisig-p2sh script-pubkey for the passed pubkey
        JADE_LOGD("Generating multisig p2sh script");
        JADE_WALLY_VERIFY(wally_scriptpubkey_p2sh_from_bytes(
            multisig_script, *written, WALLY_SCRIPT_HASH160, output, output_len, written));
    } else {
        JADE_ASSERT_MSG(false, "Unrecognised script variant: %u", script_variant);
        return false;
    }

    JADE_ASSERT(*written == script_length_for_variant(script_variant));
    return true;
}

bool wallet_search_for_multisig_script(const script_variant_t script_variant, const bool sorted,
    const uint8_t threshold, const struct ext_key* search_roots, const size_t search_roots_len, size_t* index,
    const size_t search_depth, const uint8_t* script, const size_t script_len)
{
    JADE_ASSERT(keychain_get());

    if (!is_multisig(script_variant) || !threshold || !search_roots || !search_roots_len
        || search_roots_len > MAX_ALLOWED_SIGNERS || !index || !search_depth || !script
        || script_len != script_length_for_variant(script_variant)) {
        return false;
    }

    bool found = false;
    uint8_t pubkeys[MAX_ALLOWED_SIGNERS * EC_PUBLIC_KEY_LEN]; // Sufficient
    const size_t pubkeys_len = search_roots_len * EC_PUBLIC_KEY_LEN;
    uint8_t generated[WALLY_SCRIPTPUBKEY_P2WSH_LEN]; // Sufficient
    for (const size_t end = *index + search_depth; *index < end; ++*index) {
        // Try next leaf (derive all keys)
        for (int i = 0; i < search_roots_len; ++i) {
            struct ext_key derived;
            JADE_WALLY_VERIFY(bip32_key_from_parent(
                &search_roots[i], *index, BIP32_FLAG_KEY_PUBLIC | BIP32_FLAG_SKIP_HASH, &derived));

            uint8_t* const pubkey = pubkeys + (i * EC_PUBLIC_KEY_LEN);
            memcpy(pubkey, derived.pub_key, sizeof(derived.pub_key));
        }

        // Build a standard multisig script
        size_t written = 0;
        if (!wallet_build_multisig_script(
                script_variant, sorted, threshold, pubkeys, pubkeys_len, generated, sizeof(generated), &written)) {
            JADE_LOGE("Error generating multisig script");
            return false;
        }

        // See if generated is identical to the script passed in
        if (written == script_len && !memcmp(generated, script, script_len)) {
            found = true;
            break;
        }
    }
    return found;
}

bool wallet_build_descriptor_script(const network_t network_id, const char* descriptor_name,
    const descriptor_data_t* descriptor, const size_t multi_index, const size_t index, uint8_t* output,
    const size_t output_len, size_t* written, const char** errmsg)
{
    JADE_ASSERT(keychain_get());

    if (network_id == NETWORK_NONE || !descriptor_name || !descriptor || !output || !output_len || !errmsg) {
        return false;
    }

    uint8_t* p_script = NULL;
    size_t script_len = 0;
    if (!descriptor_to_script(
            descriptor_name, descriptor, network_id, multi_index, index, NULL, &p_script, &script_len, errmsg)
        || !script_len || script_len > output_len) {
        return false;
    }

    // Copy allocated script into passed buffer and free allocation
    memcpy(output, p_script, script_len);
    *written = script_len;
    free(p_script);

    return true;
}

bool wallet_search_for_descriptor_script(const network_t network_id, const char* descriptor_name,
    const descriptor_data_t* descriptor, size_t multi_index, size_t* index, size_t search_depth, const uint8_t* script,
    const size_t script_len)
{
    JADE_ASSERT(keychain_get());

    if (!descriptor || descriptor->num_values > MAX_ALLOWED_SIGNERS || !index || !search_depth || !script) {
        return false;
    }

    uint32_t child_num = *index;
    const bool found = descriptor_search_for_script(
        descriptor_name, descriptor, network_id, multi_index, &child_num, search_depth, script, script_len);

    *index = child_num;
    return found;
}

// Function to compute an anti-exfil signer commitment with a derived key for a given
// signature hash (SHA256_LEN) and host commitment (WALLY_HOST_COMMITMENT_LEN).
// Output must be of size WALLY_S2C_OPENING_LEN.
bool wallet_get_signer_commitment(const uint8_t* signature_hash, const size_t signature_hash_len, const uint32_t* path,
    const size_t path_len, const uint8_t* commitment, const size_t commitment_len, uint8_t* output,
    const size_t output_len)
{
    if (!signature_hash || signature_hash_len != SHA256_LEN || !path || path_len == 0 || !commitment
        || commitment_len != WALLY_HOST_COMMITMENT_LEN || !output || output_len != WALLY_S2C_OPENING_LEN) {
        return false;
    }

    // Derive the child key
    uint8_t privkey[EC_PRIVATE_KEY_LEN];
    SENSITIVE_PUSH(privkey, sizeof(privkey));
    wallet_get_privkey(path, path_len, privkey, sizeof(privkey));

    // Generate the signer commitment nonce
    const int wret = wally_ae_signer_commit_from_bytes(privkey, sizeof(privkey), signature_hash, signature_hash_len,
        commitment, commitment_len, EC_FLAG_ECDSA, output, output_len);
    SENSITIVE_POP(privkey);

    if (wret != WALLY_OK) {
        JADE_LOGE("Failed to get signer commitment nonce, error %d", wret);
        return false;
    }
    return true;
}

// Function to sign an input hash with a derived key - cannot be the root key, and value must be a sha256 hash.
// If 'ae_host_entropy' is passed it is used to generate an 'anti-exfil' signature, otherwise a standard EC signature
// (ie. using rfc6979) is created.  The output signature is returned in DER format, with a SIGHASH_<xxx> postfix.
// Output buffer size must be (EC_SIGNATURE_DER_MAX_LEN + 1).
// NOTE: the standard EC signature will 'grind-r' to produce a 'low-r' signature, the anti-exfil case
// cannot (as the entropy is provided explicitly). However all signatures produced are Low-S,
// to comply with bitcoin standardness rules.
bool wallet_sign_tx_input_hash(const network_t network_id, input_data_t* input_data, const uint8_t* ae_host_entropy,
    const size_t ae_host_entropy_len)
{
    if (!input_data || input_data->path_len == 0) {
        return false;
    }
    if ((!ae_host_entropy && ae_host_entropy_len > 0)
        || (ae_host_entropy && ae_host_entropy_len != WALLY_S2C_DATA_LEN)) {
        return false;
    }

    uint8_t privkey[EC_PRIVATE_KEY_LEN];
    uint8_t signature[EC_SIGNATURE_LEN];

    // Derive the child key
    SENSITIVE_PUSH(privkey, sizeof(privkey));
    wallet_get_privkey(input_data->path, input_data->path_len, privkey, sizeof(privkey));

    // Generate signature as appropriate
    int wret = WALLY_OK;
    if (ae_host_entropy && input_data->sig_type != WALLY_SIGTYPE_SW_V1) {
        // Anti-Exfil signature
        wret = wally_ae_sig_from_bytes(privkey, sizeof(privkey), input_data->signature_hash,
            sizeof(input_data->signature_hash), ae_host_entropy, ae_host_entropy_len, EC_FLAG_ECDSA, signature,
            sizeof(signature));
    } else {
        // Standard EC or taproot signature
        uint8_t tweaked[EC_PRIVATE_KEY_LEN];
        uint8_t* signing_key;
        uint32_t flags;
        SENSITIVE_PUSH(tweaked, sizeof(tweaked));

        if (input_data->sig_type != WALLY_SIGTYPE_SW_V1) {
            // ECDSA. Sign directly with the private key
            signing_key = privkey;
            flags = EC_FLAG_ECDSA | EC_FLAG_GRIND_R;
        } else {
            // Taproot. Tweak the private key before Schnorr signing
            signing_key = tweaked;
            flags = network_is_liquid(network_id) ? EC_FLAG_ELEMENTS : 0;
            wret
                = wally_ec_private_key_bip341_tweak(privkey, sizeof(privkey), NULL, 0, flags, tweaked, sizeof(tweaked));
            flags |= EC_FLAG_SCHNORR; // Update for signing below
        }
        if (wret == WALLY_OK) {
            wret = wally_ec_sig_from_bytes(signing_key, EC_PRIVATE_KEY_LEN, input_data->signature_hash,
                sizeof(input_data->signature_hash), flags, signature, sizeof(signature));
        }
        SENSITIVE_POP(tweaked);
    }
    SENSITIVE_POP(privkey);

    if (wret != WALLY_OK) {
        JADE_LOGE("Failed to make signature, error %d", wret);
        return false;
    }

    if (input_data->sig_type != WALLY_SIGTYPE_SW_V1) {
        // ECDSA: DER-encode the signature
        JADE_WALLY_VERIFY(wally_ec_sig_to_der(
            signature, sizeof(signature), input_data->sig, sizeof(input_data->sig) - 1, &input_data->sig_len));
    } else {
        // Taproot: Copy the Schnorr signature as-is.
        input_data->sig_len = sizeof(signature);
        memcpy(input_data->sig, signature, sizeof(signature));
        if (input_data->sighash == WALLY_SIGHASH_DEFAULT) {
            // We don't add the sighash byte for default signatures, so we are done
            return true;
        }
    }

    // Append the sighash used
    JADE_ASSERT(input_data->sig_len <= sizeof(input_data->sig) - 1);
    input_data->sig[input_data->sig_len] = input_data->sighash;
    input_data->sig_len += 1;

    return true;
}

signing_data_t* signing_data_allocate(const size_t num_inputs)
{
    signing_data_t* p = JADE_CALLOC(1, sizeof(signing_data_t));
    p->inputs = JADE_CALLOC(num_inputs, sizeof(input_data_t));
    p->num_inputs = num_inputs;
    JADE_WALLY_VERIFY(wally_map_init(num_inputs, NULL, &p->amounts));
    JADE_WALLY_VERIFY(wally_map_init(num_inputs, NULL, &p->assets));
    JADE_WALLY_VERIFY(wally_map_init(num_inputs, NULL, &p->scriptpubkeys));
    // FIXME: Use a wally constant for the cache size when it is exposed
    JADE_WALLY_VERIFY(wally_map_init(16, NULL, &p->cache));
    return p;
}

void signing_data_free(void* signing_data)
{
    // Note we ignore wally errors here to ensure we free as much as possible
    signing_data_t* p = (signing_data_t*)signing_data;
    if (p->inputs) {
        wally_bzero(p->inputs, p->num_inputs * sizeof(input_data_t));
        free(p->inputs);
    }
    wally_map_clear(&p->amounts);
    wally_map_clear(&p->assets);
    wally_map_clear(&p->scriptpubkeys);
    wally_map_clear(&p->cache);
    free(p);
}

// Function to fetch a hash for signing a transaction input
bool wallet_get_tx_input_hash(struct wally_tx* tx, const size_t index, signing_data_t* signing_data,
    const uint8_t* script, size_t script_len, const uint8_t* genesis, const size_t genesis_len)
{
    const uint32_t key_version = 0;
    const uint32_t codesep = WALLY_NO_CODESEPARATOR;
    const uint8_t* annex = NULL;
    const size_t annex_len = 0;
    int wret;

    JADE_ASSERT(signing_data);
    JADE_ASSERT(index < signing_data->num_inputs);
    input_data_t* const input_data = &signing_data->inputs[index];
    wret = wally_tx_get_input_signature_hash(tx, index, &signing_data->scriptpubkeys, &signing_data->assets,
        &signing_data->amounts, script, script_len, key_version, codesep, annex, annex_len, genesis, genesis_len,
        input_data->sighash, input_data->sig_type, &signing_data->cache, input_data->signature_hash,
        sizeof(input_data->signature_hash));

    if (wret != WALLY_OK) {
        JADE_LOGE("Failed to get signature hash for segwit version %d, error %d", (int)input_data->sig_type, wret);
        return false;
    }
    return true;
}

void wallet_get_fingerprint(uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(keychain_get());
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == BIP32_KEY_FINGERPRINT_LEN);

    // Fingerprint is first 4 bytes of the hash160, which should be populated already
    memcpy(output, keychain_get()->xpriv.hash160, output_len);
}

bool wallet_get_hdkey(const uint32_t* path, const size_t path_len, const uint32_t flags, struct ext_key* output)
{
    JADE_ASSERT(keychain_get());
    JADE_ASSERT(path_len == 0 || path);

    if (!output) {
        return false;
    }

    if (path_len == 0) {
        // Just copy root ext key
        memcpy(output, &keychain_get()->xpriv, sizeof(struct ext_key));
    } else {
        const int wret = bip32_key_from_parent_path(&keychain_get()->xpriv, path, path_len, flags, output);
        if (wret != WALLY_OK) {
            JADE_LOGE("Failed to derive key from path (size %u): %d", path_len, wret);
            return false;
        }
    }

    return true;
}

bool wallet_get_xpub(const network_t network_id, const uint32_t* path, const size_t path_len, char** output)
{
    JADE_ASSERT(keychain_get());
    JADE_ASSERT(path_len == 0 || path);
    JADE_ASSERT(output);

    // Get the version prefix bytes for the passed network
    const uint32_t version = network_to_bip32_version(network_id);

    // NOTE: we do not SKIP_HASH in this case, as it is included in the xpub
    struct ext_key derived;
    if (!wallet_get_hdkey(path, path_len, BIP32_FLAG_KEY_PUBLIC, &derived)) {
        return false;
    }

    // Override network/version to yield the correct prefix for the passed network
    derived.version = version;
    JADE_WALLY_VERIFY(bip32_key_to_base58(&derived, BIP32_FLAG_KEY_PUBLIC, output));
    JADE_LOGD("bip32_key_to_base58: %s", *output);
    return true;
}

bool wallet_hmac_with_master_key(const uint8_t* data, const size_t data_len, uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(keychain_get());

    if (!data || data_len == 0 || !output || output_len != HMAC_SHA256_LEN) {
        return false;
    }

    // HMAC with the private key - note we ignore the first byte of the array as it is a prefix to the actual key
    return wally_hmac_sha256(keychain_get()->xpriv.priv_key + 1, sizeof(keychain_get()->xpriv.priv_key) - 1, data,
               data_len, output, output_len)
        == WALLY_OK;
}

// Return script blinding privkey given master blinding key (slip-0077)
static bool wallet_get_blinding_privkey(const uint8_t* master_blinding_key, const size_t master_blinding_key_len,
    const uint8_t* script, const size_t script_len, uint8_t* output, const size_t output_len)
{
    JADE_ASSERT(master_blinding_key);
    JADE_ASSERT(master_blinding_key_len == HMAC_SHA512_LEN);
    JADE_ASSERT(script);
    JADE_ASSERT(script_len);
    JADE_ASSERT(output);
    JADE_ASSERT(output_len == EC_PRIVATE_KEY_LEN);

    // NOTE: 'master_unblinding_key' passed here as the full output of hmac512, when according to slip-0077
    // the master unblinding key is only the second half of that - ie. 256 bits
    // 'wally_asset_blinding_key_to_ec_private_key()' takes this into account...
    const int wret = wally_asset_blinding_key_to_ec_private_key(
        master_blinding_key, master_blinding_key_len, script, script_len, output, output_len);
    if (wret != WALLY_OK) {
        JADE_LOGE("Error building asset blinding key for script: %d", wret);
        return false;
    }
    return true;
}

bool wallet_get_public_blinding_key(const uint8_t* master_blinding_key, const size_t master_blinding_key_len,
    const uint8_t* script, const size_t script_len, uint8_t* output, const size_t output_len)
{
    if (!master_blinding_key || master_blinding_key_len != HMAC_SHA512_LEN || !script || !script_len || !output
        || output_len != EC_PUBLIC_KEY_LEN) {
        return false;
    }

    uint8_t privkey[EC_PRIVATE_KEY_LEN];
    SENSITIVE_PUSH(privkey, sizeof(privkey));
    const bool ret = wallet_get_blinding_privkey(
        master_blinding_key, master_blinding_key_len, script, script_len, privkey, sizeof(privkey));
    if (ret) {
        JADE_WALLY_VERIFY(wally_ec_public_key_from_private_key(privkey, sizeof(privkey), output, output_len));
    }
    SENSITIVE_POP(privkey);
    return ret;
}

bool wallet_get_blinding_factor(const uint8_t* master_blinding_key, const size_t master_blinding_key_len,
    const uint8_t* hash_prevouts, const size_t hash_len, const size_t output_index, const BlindingFactorType_t type,
    uint8_t* output, const size_t output_len)
{
    if (!master_blinding_key || master_blinding_key_len != HMAC_SHA512_LEN || !hash_prevouts || hash_len != SHA256_LEN
        || !output) {
        return false;
    }

    // Map to appropriate wally function
    typedef int (*fn_get_blinding_factor_t)(const uint8_t*, size_t, const uint8_t*, size_t, uint32_t, uint8_t*, size_t);
    fn_get_blinding_factor_t get_blinding_factor = NULL;
    if (type == BF_ASSET && output_len == BLINDING_FACTOR_LEN) {
        get_blinding_factor = wally_asset_blinding_key_to_abf;
    } else if (type == BF_VALUE && output_len == BLINDING_FACTOR_LEN) {
        get_blinding_factor = wally_asset_blinding_key_to_vbf;
    } else if (type == BF_ASSET_VALUE && output_len == WALLY_ABF_VBF_LEN) {
        get_blinding_factor = wally_asset_blinding_key_to_abf_vbf;
    } else {
        return false;
    }

    // Get the blinding factor(s)
    JADE_ASSERT(get_blinding_factor);
    JADE_WALLY_VERIFY(get_blinding_factor(
        master_blinding_key, master_blinding_key_len, hash_prevouts, hash_len, output_index, output, output_len));

    return true;
}

// Compute the shared blinding nonce - ie. sha256(ecdh(our_privkey, their_pubkey))
bool wallet_get_shared_blinding_nonce(const uint8_t* master_blinding_key, const size_t master_blinding_key_len,
    const uint8_t* script, const size_t script_len, const uint8_t* their_pubkey, const size_t their_pubkey_len,
    uint8_t* output_nonce, const size_t output_nonce_len, uint8_t* output_pubkey, const size_t output_pubkey_len)
{
    if (!master_blinding_key || master_blinding_key_len != HMAC_SHA512_LEN || !script || !script_len || !their_pubkey
        || !output_nonce || output_nonce_len != SHA256_LEN) {
        return false;
    }
    if ((output_pubkey && output_pubkey_len != EC_PUBLIC_KEY_LEN) || (!output_pubkey && output_pubkey_len != 0)) {
        return false;
    }

    uint8_t privkey[EC_PRIVATE_KEY_LEN];
    SENSITIVE_PUSH(privkey, sizeof(privkey));
    if (!wallet_get_blinding_privkey(
            master_blinding_key, master_blinding_key_len, script, script_len, privkey, sizeof(privkey))) {
        SENSITIVE_POP(privkey);
        return false;
    }

    const int wret = wally_ecdh_nonce_hash(
        their_pubkey, their_pubkey_len, privkey, sizeof(privkey), output_nonce, output_nonce_len);
    if (wret != WALLY_OK) {
        JADE_LOGE("Error building ecdh nonce hash: %d", wret);
        SENSITIVE_POP(privkey);
        return false;
    }

    // Caller may also want our public blinding key
    if (output_pubkey) {
        JADE_WALLY_VERIFY(
            wally_ec_public_key_from_private_key(privkey, sizeof(privkey), output_pubkey, output_pubkey_len));
    }

    SENSITIVE_POP(privkey);
    return true;
}

bool wallet_get_message_hash(const uint8_t* bytes, const size_t bytes_len, uint8_t* output, const size_t output_len)
{
    if (!bytes || bytes_len == 0 || !output || output_len != SHA256_LEN) {
        return false;
    }

    size_t written = 0;
    const int wret
        = wally_format_bitcoin_message(bytes, bytes_len, BITCOIN_MESSAGE_FLAG_HASH, output, output_len, &written);
    if (wret != WALLY_OK || written != output_len) {
        JADE_LOGE("Error trying to format btc message: %d", wret);
        return false;
    }
    return true;
}

// Function to sign a message hash with a derived key - cannot be the root key, and value must be a sha256 hash.
// If 'ae_host_entropy' is passed it is used to generate an 'anti-exfil' signature, otherwise a standard EC
// signature (ie. using rfc6979) is created.  The output signature is returned base64 encoded. The output buffer
// should be of size at least EC_SIGNATURE_LEN * 2 (which should be ample).
bool wallet_sign_message_hash(const uint8_t* signature_hash, const size_t signature_hash_len, const uint32_t* path,
    const size_t path_len, const uint8_t* ae_host_entropy, const size_t ae_host_entropy_len, uint8_t* output,
    const size_t output_len, size_t* written)
{
    if (!path || !path_len || !signature_hash || signature_hash_len != SHA256_LEN || !output
        || output_len < EC_SIGNATURE_LEN * 2 || !written) {
        return false;
    }
    if ((!ae_host_entropy && ae_host_entropy_len > 0)
        || (ae_host_entropy && ae_host_entropy_len != WALLY_S2C_DATA_LEN)) {
        return false;
    }

    // Derive the child key
    uint8_t privkey[EC_PRIVATE_KEY_LEN];
    SENSITIVE_PUSH(privkey, sizeof(privkey));
    wallet_get_privkey(path, path_len, privkey, sizeof(privkey));

    // Generate signature as appropriate
    int wret;
    uint8_t signature[EC_SIGNATURE_RECOVERABLE_LEN];
    size_t signature_len = 0;
    if (ae_host_entropy) {
        // Anti-Exfil signature
        signature_len = EC_SIGNATURE_LEN;
        wret = wally_ae_sig_from_bytes(privkey, sizeof(privkey), signature_hash, signature_hash_len, ae_host_entropy,
            ae_host_entropy_len, EC_FLAG_ECDSA, signature, signature_len);
    } else {
        // Standard EC recoverable signature
        signature_len = EC_SIGNATURE_RECOVERABLE_LEN;
        wret = wally_ec_sig_from_bytes(privkey, sizeof(privkey), signature_hash, signature_hash_len,
            EC_FLAG_ECDSA | EC_FLAG_RECOVERABLE, signature, signature_len);
    }
    SENSITIVE_POP(privkey);

    if (wret != WALLY_OK) {
        JADE_LOGE("Failed to make signature, error %d", wret);
        return false;
    }
    // Base64 encode
    JADE_ZERO_VERIFY(mbedtls_base64_encode(output, output_len, written, signature, signature_len));
    JADE_ASSERT(*written < output_len);
    output[*written] = '\0';
    *written += 1;

    return true;
}

// Function to get bip85-generated entropy for a new bip39 mnemonic
// See: https://github.com/bitcoin/bips/blob/master/bip-0085.mediawiki
// NOTE: only the English wordlist is supported.
void wallet_get_bip85_bip39_entropy(
    const size_t nwords, const size_t index, uint8_t* entropy, const size_t entropy_len, size_t* written)
{
    JADE_ASSERT(nwords);
    JADE_ASSERT(entropy);
    JADE_ASSERT(entropy_len == HMAC_SHA512_LEN);
    JADE_INIT_OUT_SIZE(written);

    JADE_ASSERT(keychain_get());

    // Get bip85 path for bip39 mnemonic
    JADE_WALLY_VERIFY(
        bip85_get_bip39_entropy(&keychain_get()->xpriv, NULL, nwords, index, entropy, entropy_len, written));
    JADE_ASSERT(*written <= entropy_len);
}

// Function to get bip85-generated entropy for a new rsa key
// See: https://github.com/bitcoin/bips/blob/master/bip-0085.mediawiki
void wallet_get_bip85_rsa_entropy(
    const size_t key_bits, const size_t index, uint8_t* entropy, const size_t entropy_len, size_t* written)
{
    JADE_ASSERT(key_bits);
    JADE_ASSERT(entropy);
    JADE_ASSERT(entropy_len == HMAC_SHA512_LEN);
    JADE_INIT_OUT_SIZE(written);

    JADE_ASSERT(keychain_get());

    JADE_WALLY_VERIFY(bip85_get_rsa_entropy(&keychain_get()->xpriv, key_bits, index, entropy, entropy_len, written));
    JADE_ASSERT(*written <= entropy_len);
}
#endif // AMALGAMATED_BUILD
