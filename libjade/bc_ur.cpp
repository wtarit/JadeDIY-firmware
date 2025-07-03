#include <cstring>

#include "./bc-ur-0.3.0/src/bytewords.cpp"
#include "./bc-ur-0.3.0/src/fountain-decoder.cpp"
#include "./bc-ur-0.3.0/src/fountain-encoder.cpp"
#include "./bc-ur-0.3.0/src/fountain-utils.cpp"
#include "./bc-ur-0.3.0/src/random-sampler.cpp"
#include "./bc-ur-0.3.0/src/ur-decoder.cpp"
#include "./bc-ur-0.3.0/src/ur-encoder.cpp"
#include "./bc-ur-0.3.0/src/ur.cpp"
#include "./bc-ur-0.3.0/src/utils.cpp"
#include "./bc-ur-0.3.0/src/xoshiro256.cpp"

extern "C" {

// Encode: C API
void urcreate_encoder(void** const encoder, const char* type, const uint8_t* cbor, size_t cbor_len,
    size_t max_fragment_len, uint32_t first_seq_num, size_t min_fragment_len)
{
    assert(encoder && !*encoder && type && ur::is_ur_type(type) && cbor && cbor_len && max_fragment_len
        && min_fragment_len && max_fragment_len > min_fragment_len);
    ur::ByteVector bv(cbor, cbor + cbor_len);
    ur::UR ur(type, bv);
    *encoder = new (std::nothrow) ur::UREncoder(ur, max_fragment_len, first_seq_num, min_fragment_len);
    assert(*encoder);
}

void urcreate_placement_encoder(void* const encoder, size_t encoder_len, const char* type, const uint8_t* cbor,
    size_t cbor_len, size_t max_fragment_len, uint32_t first_seq_num, size_t min_fragment_len)
{
    assert(encoder && encoder_len == sizeof(ur::UREncoder) && type && ur::is_ur_type(type) && cbor && cbor_len
        && max_fragment_len && min_fragment_len && max_fragment_len > min_fragment_len);
    ur::ByteVector bv(cbor, cbor + cbor_len);
    ur::UR ur(type, bv);
    void* tmp = new (encoder) ur::UREncoder(ur, max_fragment_len, first_seq_num, min_fragment_len);
    assert(tmp);
}

void urfree_encoder(void* const encoder)
{
    if (encoder) {
        ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
        delete urencoder;
    }
}

void urfree_placement_encoder(void* const encoder)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    urencoder->~UREncoder();
}

uint32_t urseqnum_encoder(void* const encoder)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    return urencoder->seq_num();
}

uint32_t urseqlen_encoder(void* const encoder)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    return urencoder->seq_len();
}

bool uris_complete_encoder(void* const encoder)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    return urencoder->is_complete();
}

bool uris_single_part_encoder(void* const encoder)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    return urencoder->is_single_part();
}

// NOTE: changed to not use strlcpy
void urnext_part_encoder(void* const encoder, const bool force_uppercase, char** next)
{
    assert(encoder);
    ur::UREncoder* urencoder = (ur::UREncoder*)encoder;
    std::string encoded = urencoder->next_part();
    (*next) = (char*)malloc(encoded.size() + 1);
    if (force_uppercase) {
        std::transform(encoded.begin(), encoded.end(), *next, ::toupper);
    } else {
        std::copy(encoded.begin(), encoded.end(), *next);
    }
    (*next)[encoded.size()] = '\0'; // ensure terminated
}

void urfree_encoded_encoder(char* encoded) { free(encoded); }

// Decode: C API

void urcreate_decoder(void** const decoder)
{
    assert(decoder && !*decoder);
    *decoder = new (nothrow) ur::URDecoder();
    assert(*decoder);
}

void urcreate_placement_decoder(void* const decoder, size_t decoder_len)
{
    assert(decoder && decoder_len == sizeof(ur::URDecoder));
    void* tmp = new (decoder) ur::URDecoder();
    assert(tmp);
}

void urfree_decoder(void* const decoder)
{
    if (decoder) {
        ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
        delete urdecoder;
    }
}

void urfree_placement_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    urdecoder->~URDecoder();
}

bool urreceive_part_decoder(void* const decoder, const char* s)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    const string part(s);
    return urdecoder->receive_part(part);
}

size_t urprocessed_parts_count_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->processed_parts_count();
}

size_t urreceived_parts_count_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->received_part_indexes().size();
}

size_t urexpected_part_count_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->expected_part_count();
}

double urestimated_percent_complete_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->estimated_percent_complete();
}

bool uris_success_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->is_success();
}

bool uris_failure_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->is_failure();
}

bool uris_complete_decoder(void* const decoder)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    return urdecoder->is_complete();
}

void urresult_ur_decoder(void* const decoder, uint8_t** result, size_t* result_len, const char** type)
{
    assert(decoder);
    ur::URDecoder* urdecoder = (ur::URDecoder*)decoder;
    const ur::UR& ur = urdecoder->result_ur();
    (*type) = ur.type().c_str();
    const ur::ByteVector& res = ur.cbor();
    (*result) = (uint8_t*)res.data();
    (*result_len) = res.size();
}

} // extern "C"
