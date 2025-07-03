// A single source file containing a local implementation of the Jade Firmware
//
// This hack is designed for local development, debugging and testing.
//
// WARNING: THIS CODE IS NOT SUITABLE FOR PROCESSING REAL DATA.
//          DO NOT USE THIS CODE FOR ANY PURPOSE WITH NON-TEST DATA.
//          DOING SO IS INSECURE AND MAY RESULT IN THE LOSS OF FUNDS!
//
// Includes the entire Jade firmware code, replacing the GUI and most
// of the OS support code.
// This file can be compiled to a shared library which implements an
// in-processes software Jade emulator/virtual Jade device.
// The exposed API allows passing and fetching messages using the same
// binary format that would be passed to a real device by serial/bluetooth.
//
#define _GNU_SOURCE 1 // FIXME: needed for strcasestr in qrmode.c
#include <string.h>
#undef _GNU_SOURCE
#include "sdkconfig.h"

// Prevent secp symbols being externally visible in our final shared library
#define SECP256K1_API

// Address sanitizer doesn't like calling sha256 into a non-aligned
// buffer, even though its technically legal (but slower). Force
// wally to handle unaligned destination buffers internally to work
// around this.
#define HAVE_UNALIGNED_ACCESS 0

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h> // Must be included before we redefine settimeofday()

// Include the tinycbor sources we need for CBOR processing
#include "managed_components/espressif__cbor/tinycbor/src/cborencoder.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborparser.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborparser_dup_string.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborpretty.c"
#include "managed_components/espressif__cbor/tinycbor/src/cborpretty_stdio.c"
#include "managed_components/espressif__cbor/tinycbor/src/cbortojson.c"
// Include the asset snapshot component sources
#include "components/assets/assets_snapshot.c"
// qrCode encoding/decoding
#include "components/esp32-quirc/openmv/collections.c"
#include "components/esp32-quirc/lib/quirc.c"
#include "components/esp32-quirc/lib/decode.c"
#include "components/esp32-quirc/lib/identify.c"
#include "components/esp32-quirc/lib/version_db.c"

// abort is mapped to __wrap_abort in the firmware. This calls jade_abort,
// which calls __real_abort, which we implement as calling (the real) abort
static void __real_abort(void) { abort(); }

// Make time setting a no-op.
// We are in the same process as the caller, so anything that depends on the
// current time will automatically match the callers time.
static int settimeofday_no_op(const void* yv, const void* tz) { return 0; }
#define settimeofday settimeofday_no_op

// Include the core Jade firmware core, including wally/secp.
#define AMALGAMATED_BUILD
#include "main/amalgamated.c"
#undef settimeofday

// GUI: Include our fake GUI
#include "gui.c"

// Include the bc-ur C parts which can't be compiled as cpp
#include "bc-ur-0.3.0/src/crc32.c"
#include "bc-ur-0.3.0/src/memzero.c"
void sha256_Raw(const unsigned char* data, size_t len, uint8_t digest[SHA256_LEN])
{
    if (wally_sha256(data, len, digest, SHA256_LEN) != WALLY_OK) {
        abort();
    }
}

//
// Stubs for code that does not apply to libjade or is not yet implemented
//

// main/idletimer.c
void idletimer_init(void) {}
bool idletimer_register_activity(const bool is_ui) { return true; }
void idletimer_set_min_timeout_secs(uint16_t min_timeout_secs){};

// main/logging.c
esp_log_level_t _libjade_log_level = ESP_LOG_NONE;

// main/otpauth.c
bool otp_is_valid(const otpauth_ctx_t* otp_ctx) { return true; }

bool otp_uri_to_ctx(const char* uri, size_t uri_len, otpauth_ctx_t* otp_ctx) { return false; }

void otp_set_explicit_value(otpauth_ctx_t* otp_ctx, int64_t value) {}
bool otp_set_default_value(otpauth_ctx_t* otp_ctx, uint64_t* value_out) { return false; }

bool otp_get_auth_code(const otpauth_ctx_t* otp_ctx, char* token, size_t token_len) { return false; }

bool otp_save_uri(const char* otp_name, const char* uri, size_t uri_len) { return false; }
bool otp_load_uri(const char* otp_name, char* uri, size_t uri_len, size_t* written) { return false; }

// main/selfcheck.c
bool debug_selfcheck(jade_process_t* process) { return true; }

esp_app_desc_t running_app_info = { "123456789012345678901" };
esp_chip_info_t chip_info = { 0 };

void esp_restart() { abort(); }

const char* esp_get_idf_version(void) { return "9.9.99-99-fake_hack"; }

void esp_chip_info(esp_chip_info_t* out)
{
    out->features = 0; // FIXME
}

esp_err_t esp_efuse_mac_get_default(uint8_t* out)
{
    memset(out, 0, 6);
    return ESP_OK;
}

void esp_deep_sleep_start(void) { abort(); }

// UI
uint8_t GUI_DEFAULT_FONT = 0;
uint8_t GUI_TITLE_FONT = 1;

// Display
void display_init(TaskHandle_t* task) {}
void input_init(void) {}
bool serial_init(TaskHandle_t* task) { return true; }

// Camera
uint16_t camera_displayed_image_width(void) { return (CONFIG_DISPLAY_WIDTH * 70 / 100); }

uint16_t camera_displayed_image_height(void) { return CONFIG_DISPLAY_HEIGHT; }

// main/ui/keyboard.c
void make_keyboard_entry_activity(keyboard_entry_t* kb_entry, const char* title) {}

void run_keyboard_entry_loop(keyboard_entry_t* kb_entry) {}

// Common process handler for stuff we don't support yet
static void unsupported_process(void* process, char* name)
{
    JADE_LOGE("%s not supported by Software Jade", name);
    jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "Unsupported", NULL);
}
#define UNSUPPORTED(n) void n##_process(void* p) { unsupported_process(p, #n); }

UNSUPPORTED(debug_scan_qr)
UNSUPPORTED(get_bip85_bip39_entropy)
UNSUPPORTED(get_bip85_rsa_entropy)
int get_bip85_bip39_entropy_cbor(const CborValue* params, CborEncoder* output, const char** errmsg)
{
    *errmsg = "User declined to export entropy";
    return CBOR_RPC_USER_CANCELLED;
}
UNSUPPORTED(get_bip85_pubkey)
UNSUPPORTED(get_identity_pubkey)
UNSUPPORTED(get_identity_shared_key)
UNSUPPORTED(ota)
UNSUPPORTED(ota_delta)
UNSUPPORTED(sign_bip85_digests)
UNSUPPORTED(sign_identity)

const uint8_t _binary_pinserver_public_key_pub_start[33]
    = { 0x03, 0x32, 0xb7, 0xb1, 0x34, 0x8b, 0xde, 0x8c, 0xa4, 0xb4, 0x6b, 0x9d, 0xcc, 0x30, 0x32, 0x0e, 0x14, 0x0c,
          0xa2, 0x64, 0x28, 0x16, 0x0a, 0x27, 0xbd, 0xbf, 0xc3, 0x0b, 0x34, 0xec, 0x87, 0xc5, 0x47 };

static const uint8_t empty_byte;

const uint8_t* _binary_icon_qrguide_qvga_large_bin_gz_start = &empty_byte;
const uint8_t* _binary_icon_qrguide_qvga_large_bin_gz_end = &empty_byte + 1;
const uint8_t* _binary_icon_qrguide_qvga_small_bin_gz_start = &empty_byte;
const uint8_t* _binary_icon_qrguide_qvga_small_bin_gz_end = &empty_byte + 1;

Icon* get_icon(const uint8_t* const start, const uint8_t* const end) { return NULL; }

// Events
static volatile bool _libjade_stop_requested = false; // Used to stop the firmware

void sync_wait_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data) {}

esp_err_t sync_wait_event(wait_event_data_t* wait_event_data, esp_event_base_t* trigger_event_base,
    int32_t* trigger_event_id, void** trigger_event_data, TickType_t max_wait)
{
    if (_libjade_stop_requested) {
        // User requested the firmware to exit
        pthread_exit(NULL);
    }
    return ESP_NO_EVENT;
}

esp_err_t sync_await_single_event(esp_event_base_t event_base, int32_t event_id, esp_event_base_t* trigger_event_base,
    int32_t* trigger_event_id, void** trigger_event_data, TickType_t max_wait)
{
    return ESP_OK;
}

// HW: Task API
bool run_on_temporary_stack(size_t stack_size, temporary_stack_function_t fn, void* ctx) { return fn(ctx); }

bool run_in_temporary_task(const size_t stack_size, temporary_stack_function_t fn, void* ctx) { return fn(ctx); }

void temp_stack_init(void) {}

// HW: TLS/Sensitive
static void* _tls_ptrs[3];

void* pvTaskGetThreadLocalStoragePointer(void* task, size_t idx)
{
    assert(idx <= sizeof(_tls_ptrs) / sizeof(_tls_ptrs[0]));
    return _tls_ptrs[idx];
}

void vTaskSetThreadLocalStoragePointerAndDelCallback(void* task, size_t idx, void* p, TlsDeleteCallbackFunction_t cb)
{
    assert(idx <= sizeof(_tls_ptrs) / sizeof(_tls_ptrs[0]));
    _tls_ptrs[idx] = p;
    // FIXME: call cb atexit()/ thread exit?
}

const char* pcTaskGetName(void* task) { return "shim_task"; }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t func, const char* name, uint32_t stack_size, void* params,
    uint32_t ux_prio, TaskHandle_t* output, uint32_t xCoreID)
{
    *output = NULL;
    func(params);
    return pdTRUE;
}

unsigned int uxTaskGetStackHighWaterMark(void* task) { return 0xffffff; }

unsigned int xPortGetFreeHeapSize(void) { return 0xffffff; }

void vTaskDelay(unsigned int delay)
{
    // Don't delay, since we don't have multiple threads running
    // in the firmware to wait on.
}

void vTaskDelete(void* task)
{
    // Don't delay, since we didn't create any task
}

unsigned int xTaskGetTickCount(void) { return 1000; }

int xTaskNotify(TaskHandle_t task, unsigned int v, int action) { return pdTRUE; }

void sensitive_init(void) {}

void sensitive_push(const char* file, int line, void* addr, size_t size) {}

void sensitive_pop(const char* file, int line, void* addr) {}

void sensitive_assert_empty(void) {}

void sensitive_clear_stack(void) {}

// HW: Random
void get_random(void* bytes_out, size_t len)
{
    // FIXME
    memset(bytes_out, 1, len);
}

void refeed_entropy(const void* additional, size_t len)
{
    // FIXME
}

uint8_t get_uniform_random_byte(uint8_t upper_bound)
{
    uint8_t ret;
    get_random(&ret, sizeof(ret));
    return ret % upper_bound; // Not used for crypto, so return a biased byte
}

void random_start_collecting(void) {}

void random_full_initialization(void) {}

// HW: NVS storage
static struct wally_map nvs_storage[5]; // Map of field name to contents

esp_err_t nvs_flash_init(void) { return ESP_OK; }

static struct wally_map* get_nvs_ns(const char* ns)
{
    if (!strcmp(ns, DEFAULT_NAMESPACE)) {
        return &nvs_storage[0];
    }
    if (!strcmp(ns, MULTISIG_NAMESPACE)) {
        return &nvs_storage[1];
    }
    if (!strcmp(ns, DESCRIPTOR_NAMESPACE)) {
        return &nvs_storage[2];
    }
    if (!strcmp(ns, OTP_NAMESPACE)) {
        return &nvs_storage[3];
    }
    if (!strcmp(ns, HOTP_COUNTERS_NAMESPACE)) {
        return &nvs_storage[4];
    }
    return NULL;
}

esp_err_t nvs_open(const char* ns, nvs_open_mode_t open_mode, nvs_handle_t* out_handle)
{
    *out_handle = get_nvs_ns(ns);
    return *out_handle ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length)
{
    int ret = wally_map_replace(handle, (const unsigned char*)key, strlen(key), value, length);
    return ret == WALLY_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length)
{
    const struct wally_map_item* item = wally_map_get(handle, (const unsigned char*)key, strlen(key));
    if (!item || item->value_len > *length) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(out_value, item->value, item->value_len);
    *length = item->value_len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value)
{
    return nvs_set_blob(handle, key, value, strlen(value) + 1); // Include NUL terminator
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length)
{
    return nvs_get_blob(handle, key, out_value, length);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value)
{
    // FIXME: endianess, if we will allow loading/saving flash
    return nvs_set_blob(handle, key, (void*)&value, sizeof(value));
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out_value)
{
    // FIXME: endianess, if we will allow loading/saving flash
    size_t length = sizeof(out_value);
    return nvs_get_blob(handle, key, (void*)out_value, &length);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key)
{
    if (wally_map_remove(handle, (const unsigned char*)key, strlen(key)) != WALLY_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_entry_find(const char* part_name, const char* ns, nvs_type_t type, nvs_iterator_t* output_iterator)
{
    *output_iterator = malloc(sizeof(**output_iterator));
    if (!*output_iterator) {
        return ESP_FAIL;
    }
    if (!((*output_iterator)->m = get_nvs_ns(ns)) || !(*output_iterator)->m->num_items) {
        goto fail;
    }
    // FIXME: Ignores type, pretty sure we only store the same type in each map?
    (*output_iterator)->idx = 0;
    return ESP_OK;
fail:
    free(*output_iterator);
    *output_iterator = NULL;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_entry_next(nvs_iterator_t* iterator)
{
    if ((*iterator)->idx >= (*iterator)->m->num_items) {
        nvs_release_iterator(*iterator);
        *iterator = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ++(*iterator)->idx;
    return ESP_OK;
}

esp_err_t nvs_entry_info(const nvs_iterator_t iterator, nvs_entry_info_t* out_info)
{
    // FIXME: Only sets key, as thats all we ever read
    if (!iterator || iterator->idx >= iterator->m->num_items) {
        return ESP_ERR_INVALID_ARG;
    }
    const struct wally_map_item* item = iterator->m->items + iterator->idx;
    if (item->key_len >= NVS_KEY_NAME_MAX_SIZE) {
        abort();
    }
    memcpy(out_info->key, item->key, item->key_len);
    out_info->key[item->key_len] = '\0';
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t iterator)
{
    if (iterator) {
        free(iterator);
    }
}

esp_err_t nvs_flash_erase(void)
{
    for (size_t i = 0; i < sizeof(nvs_storage) / sizeof(nvs_storage[0]); ++i) {
        wally_map_clear(&nvs_storage[i]);
    }
    return ESP_OK;
}

esp_err_t nvs_get_stats(const char* part_name, nvs_stats_t* nvs_stats)
{
    nvs_stats->used_entries = 0;
    for (size_t i = 0; i < sizeof(nvs_storage) / sizeof(nvs_storage[0]); ++i) {
        nvs_stats->used_entries += nvs_storage[i].num_items;
    }
    nvs_stats->free_entries = ESP_NVS_TOTAL_ENTRIES - nvs_stats->used_entries;
    return ESP_OK;
}

static void* jade_fw_thread_fn(void* arg)
{
    start_dashboard();
    return NULL; // Never reached
}

// External API:
static pthread_t _libjade_thread_id; // Thread ID of the FW thread

// - Start the software Jade (runs in an internal thread)
__attribute__((__visibility__("default"))) void libjade_start(void)
{
    ensure_boot_flags();
    random_start_collecting();
    validate_running_image();
    boot_process();
    sensitive_assert_empty();
    pthread_create(&_libjade_thread_id, NULL, &jade_fw_thread_fn, NULL);
}

// - Stop the software Jade
__attribute__((__visibility__("default"))) void libjade_stop(void)
{
    // Request the firmware to stop
    _libjade_stop_requested = true;
    pthread_join(_libjade_thread_id, NULL);
    _libjade_stop_requested = false;
    vRingbufferDelete(shared_in);
    shared_in = NULL;
    vRingbufferDelete(serial_out);
    serial_out = NULL;
}

static uint8_t _libjade_serial_data_out[MAX_OUTPUT_MSG_SIZE] = { 0 };
static size_t _libjade_serial_read_ptr = 0;

// - Send a message to the software Jade
__attribute__((__visibility__("default"))) bool libjade_send(const uint8_t* data, size_t size)
{
    // Pass the message through as though it came from the serial interface
    uint8_t* data_with_source = malloc(size + 1);
    JADE_ASSERT(data_with_source);
    data_with_source[0] = SOURCE_SERIAL;
    memcpy(data_with_source + 1, data, size);
    TickType_t last_processing_time = 0;
    const bool force_reject_if_no_msg = false;
    handle_data(data_with_source, &_libjade_serial_read_ptr, size, &last_processing_time, force_reject_if_no_msg,
        _libjade_serial_data_out);
    free(data_with_source);
    return true;
}

// - Receive a message from the software Jade
// Caller must free the result with libjade_release()
__attribute__((__visibility__("default"))) uint8_t* libjade_receive(unsigned int timeout, size_t* size_out)
{
    // timeout is in seconds, convert to milliseconds
    timeout *= 1000;
    void* item = xRingbufferReceive(serial_out, size_out, timeout / portTICK_PERIOD_MS);
    if (!item) {
        // No message available
        *size_out = 0;
    }
    return item;
}

// - Release a message received from libjade_receive()
__attribute__((__visibility__("default"))) void libjade_release(uint8_t* data)
{
    vRingbufferReturnItem(serial_out, data);
}

// - Set the log level for debug logging (to stdout)
// levels are 0-4 in decreasing verbosity, or 5 to disable logging
__attribute__((__visibility__("default"))) void libjade_set_log_level(int level)
{
    // Note we don't bother about thread safety for _libjade_log_level
    if (level < 0) {
        _libjade_log_level = ESP_LOG_VERBOSE;
    } else if (level >= ESP_LOG_NONE) {
        _libjade_log_level = ESP_LOG_NONE;
    } else {
        _libjade_log_level = (esp_log_level_t)level;
    }
}
