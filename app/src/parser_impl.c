/*******************************************************************************
*  (c) 2019 Zondax GmbH
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include <zxmacros.h>
#include "parser_impl.h"
#include "parser_txdef.h"
#include "cbor.h"
#include "app_mode.h"
#include "zxformat.h"

parser_tx_t parser_tx_obj;

__Z_INLINE parser_error_t parser_mapCborError(CborError err);

#define CHECK_CBOR_MAP_ERR(CALL) { \
    CborError err = CALL;  \
    if (err!=CborNoError) return parser_mapCborError(err);}

#define PARSER_ASSERT_OR_ERROR(CALL, ERROR) if (!(CALL)) return ERROR;

#define CHECK_CBOR_TYPE(type, expected) {if ((type)!=(expected)) return parser_unexpected_type;}

#define INIT_CBOR_PARSER(c, it)  \
    CborParser parser;           \
    CHECK_CBOR_MAP_ERR(cbor_parser_init((c)->buffer + (c)->offset, (c)->bufferLen - (c)->offset, 0, &parser, &(it)))

parser_error_t parser_init_context(parser_context_t *ctx,
                                   const uint8_t *buffer,
                                   uint16_t bufferSize) {
    ctx->offset = 0;
    ctx->buffer = NULL;
    ctx->bufferLen = 0;

    if (bufferSize == 0 || buffer == NULL) {
        // Not available, use defaults
        return parser_init_context_empty;
    }

    ctx->buffer = buffer;
    ctx->bufferLen = bufferSize;
    return parser_ok;
}

parser_error_t parser_init(parser_context_t *ctx, const uint8_t *buffer, uint16_t bufferSize) {
    CHECK_PARSER_ERR(parser_init_context(ctx, buffer, bufferSize))
    return parser_ok;
}

__Z_INLINE parser_error_t parser_mapCborError(CborError err) {
    switch (err) {
        case CborErrorUnexpectedEOF:
            return parser_cbor_unexpected_EOF;
        case CborErrorMapNotSorted:
            return parser_cbor_not_canonical;
        case CborNoError:
            return parser_ok;
        default:
            return parser_cbor_unexpected;
    }
}

const char *parser_getErrorDescription(parser_error_t err) {
    switch (err) {
        // General errors
        case parser_ok:
            return "No error";
        case parser_no_data:
            return "No more data";
        case parser_init_context_empty:
            return "Initialized empty context";
        case parser_display_idx_out_of_range:
            return "display_idx_out_of_range";
        case parser_display_page_out_of_range:
            return "display_page_out_of_range";
        case parser_unexepected_error:
            return "Unexepected internal error";
            // cbor
        case parser_cbor_unexpected:
            return "unexpected CBOR error";
        case parser_cbor_not_canonical:
            return "CBOR was not in canonical order";
        case parser_cbor_unexpected_EOF:
            return "Unexpected CBOR EOF";
            // Coin specific
        case parser_unexpected_tx_version:
            return "tx version is not supported";
        case parser_unexpected_type:
            return "Unexpected data type";
        case parser_unexpected_method:
            return "Unexpected method";
        case parser_unexpected_buffer_end:
            return "Unexpected buffer end";
        case parser_unexpected_value:
            return "Unexpected value";
        case parser_unexpected_number_items:
            return "Unexpected number of items";
        case parser_unexpected_characters:
            return "Unexpected characters";
        case parser_unexpected_field:
            return "Unexpected field";
        case parser_value_out_of_range:
            return "Value out of range";
        case parser_invalid_address:
            return "Invalid address format";
            /////////// Context specific
        case parser_context_mismatch:
            return "context prefix is invalid";
        case parser_context_unexpected_size:
            return "context unexpected size";
        case parser_context_invalid_chars:
            return "context invalid chars";
            // Required fields error
        case parser_required_nonce:
            return "Required field nonce";
        case parser_required_method:
            return "Required field method";
        default:
            return "Unrecognized error code";
    }
}

__Z_INLINE parser_error_t readAddress(address_t *address, CborValue *value) {
    CHECK_CBOR_TYPE(cbor_value_get_type(value), CborByteStringType)

    CborValue dummy;
    MEMZERO(address, sizeof(address_t));
    address->len = sizeof_field(address_t, buffer);

    PARSER_ASSERT_OR_ERROR(cbor_value_is_byte_string(value), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_copy_byte_string(value, (uint8_t *) address->buffer, &address->len, &dummy))

    // Addresses are at least 2 characters Protocol + random data
    PARSER_ASSERT_OR_ERROR(address->len > 1, parser_invalid_address)

    // Verify size and protocol
    switch (address->buffer[0]) {
        case ADDRESS_PROTOCOL_ID:
            // protocol 0
            PARSER_ASSERT_OR_ERROR(address->len - 1 < 21, parser_invalid_address)
            break;
        case ADDRESS_PROTOCOL_SECP256K1:
            // protocol 1
            PARSER_ASSERT_OR_ERROR(address->len - 1 == ADDRESS_PROTOCOL_SECP256K1_PAYLOAD_LEN, parser_invalid_address)
            break;
        case ADDRESS_PROTOCOL_ACTOR:
            // protocol 2
            PARSER_ASSERT_OR_ERROR(address->len - 1 == ADDRESS_PROTOCOL_ACTOR_PAYLOAD_LEN, parser_invalid_address)
            break;
        case ADDRESS_PROTOCOL_BLS:
            // protocol 3
            PARSER_ASSERT_OR_ERROR(address->len - 1 == ADDRESS_PROTOCOL_BLS_PAYLOAD_LEN, parser_invalid_address)
            break;
        default:
            return parser_invalid_address;
    }

    return parser_ok;
}

__Z_INLINE parser_error_t readBigInt(bigint_t *bigint, CborValue *value) {
    CHECK_CBOR_TYPE(cbor_value_get_type(value), CborByteStringType)
    CborValue dummy;

    MEMZERO(bigint, sizeof(bigint_t));
    bigint->len = sizeof_field(bigint_t, buffer);

    PARSER_ASSERT_OR_ERROR(cbor_value_is_byte_string(value), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_copy_byte_string(value, (uint8_t *) bigint->buffer, &bigint->len, &dummy))

    // We have an empty value so value is default (zero)
    PARSER_ASSERT_OR_ERROR(bigint->len != 0, parser_ok)

    // We only have a byte sign, no good
    PARSER_ASSERT_OR_ERROR(bigint->len > 1, parser_unexpected_value)

    // negative bigint, should be positive
    PARSER_ASSERT_OR_ERROR(bigint->buffer[0] == 0x00, parser_unexpected_value)

    return parser_ok;
}

parser_error_t printValue(const struct CborValue *value,
                          char *outVal, uint16_t outValLen,
                          uint8_t pageIdx, uint8_t *pageCount) {
    uint8_t buff[200];
    size_t buffLen = sizeof(buff);
    MEMZERO(buff, sizeof(buff));

    snprintf(outVal, outValLen, "-- EMPTY --");

    switch (value->type) {
        case CborByteStringType: {
            CHECK_CBOR_MAP_ERR(cbor_value_copy_byte_string(value, buff, &buffLen, NULL /* next */))
            CHECK_APP_CANARY()

            if (buffLen > 0) {
                char hexStr[401];
                MEMZERO(hexStr, sizeof(hexStr));
                size_t count = array_to_hexstr(hexStr, sizeof(hexStr), buff, buffLen);
                PARSER_ASSERT_OR_ERROR(count == buffLen * 2, parser_value_out_of_range)
                CHECK_APP_CANARY()

                pageString(outVal, outValLen, hexStr, pageIdx, pageCount);
                CHECK_APP_CANARY()
            }
            break;
        }
        case CborTextStringType: {
            CHECK_CBOR_MAP_ERR(cbor_value_copy_text_string(value, (char *) buff, &buffLen, NULL /* next */))
            CHECK_APP_CANARY()

            if (buffLen >= 0) {
                pageString(outVal, outValLen, (char *) buff, pageIdx, pageCount);
            }
            break;
        }
        case CborIntegerType: {
            int64_t paramValue = 0;
            CHECK_CBOR_MAP_ERR(cbor_value_get_int64_checked(value, &paramValue))
            int64_to_str(outVal, outValLen, paramValue);
            break;
        }
        default:
            snprintf(outVal, outValLen, "Type: %d", value->type);
    }
    return parser_ok;
}

parser_error_t _printParam(const parser_tx_t *tx, uint8_t paramIdx,
                           char *outVal, uint16_t outValLen,
                           uint8_t pageIdx, uint8_t *pageCount) {
    CHECK_APP_CANARY()

    if (paramIdx >= tx->numparams) {
        return parser_value_out_of_range;
    }

    CborParser parser;
    CborValue itContainer;
    CHECK_CBOR_MAP_ERR(cbor_parser_init(tx->params, MAX_PARAMS_BUFFER_SIZE, 0, &parser, &itContainer))
    CHECK_APP_CANARY()

    CborValue itParams = itContainer;

    /// Enter container?
    if (itContainer.type == CborMapType || itContainer.type == CborArrayType) {
        CHECK_CBOR_MAP_ERR(cbor_value_enter_container(&itContainer, &itParams))
        CHECK_APP_CANARY()
        for (uint8_t i = 0; i < paramIdx; ++i) {
            CHECK_CBOR_MAP_ERR(cbor_value_advance(&itParams))
            CHECK_APP_CANARY()
        }
    }

    CHECK_PARSER_ERR(printValue(&itParams, outVal, outValLen, pageIdx, pageCount))

    /// Leave container
    if (itContainer.type == CborMapType || itContainer.type == CborArrayType) {
        while (!cbor_value_at_end(&itParams)) {
            CHECK_CBOR_MAP_ERR(cbor_value_advance(&itParams))
        }
        CHECK_CBOR_MAP_ERR(cbor_value_leave_container(&itContainer, &itParams))
        CHECK_APP_CANARY()
    }

    return parser_ok;
}

parser_error_t checkMethod(uint64_t methodValue) {
    if (methodValue <= MAX_SUPPORT_METHOD) {
        return parser_ok;
    }

    return parser_unexpected_method;
}

__Z_INLINE parser_error_t readMethod(parser_tx_t *tx, CborValue *value) {

    uint64_t methodValue;
    PARSER_ASSERT_OR_ERROR(cbor_value_is_unsigned_integer(value), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_get_uint64(value, &methodValue))

    tx->numparams = 0;
    MEMZERO(tx->params, sizeof(tx->params));

    CHECK_PARSER_ERR(checkMethod(methodValue))

    if (methodValue == 0) {
        PARSER_ASSERT_OR_ERROR(value->type != CborInvalidType, parser_unexpected_type)
        CHECK_CBOR_MAP_ERR(cbor_value_advance(value))
        CHECK_CBOR_TYPE(value->type, CborByteStringType)

        size_t arraySize;
        PARSER_ASSERT_OR_ERROR(cbor_value_is_byte_string(value) || cbor_value_is_text_string(value),
                               parser_unexpected_type)
        CHECK_CBOR_MAP_ERR(cbor_value_get_string_length(value, &arraySize))

        // method0 should have zero arguments
        PARSER_ASSERT_OR_ERROR(arraySize == 0, parser_unexpected_number_items)
        tx->method = 0;

        return parser_ok;
    }

    // This area reads the entire params byte string (if present) into the txn->params
    // and sets txn->numparams to the number of params within cbor container
    // Parsing of the individual params is deferred until the display stage

    PARSER_ASSERT_OR_ERROR(cbor_value_is_valid(value), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(value))
    CHECK_CBOR_TYPE(value->type, CborByteStringType)

    PARSER_ASSERT_OR_ERROR(cbor_value_is_byte_string(value), parser_unexpected_type)

    size_t paramsBufferSize = 0;
    CHECK_CBOR_MAP_ERR(cbor_value_get_string_length(value, &paramsBufferSize))
    PARSER_ASSERT_OR_ERROR(paramsBufferSize <= sizeof(tx->params), parser_unexpected_number_items)

    // short-circuit if there are no params
    if (paramsBufferSize != 0) {
        size_t paramsLen = sizeof(tx->params);
        CHECK_CBOR_MAP_ERR(cbor_value_copy_byte_string(value, tx->params, &paramsLen, NULL /* next */))
        PARSER_ASSERT_OR_ERROR(paramsLen <= sizeof(tx->params), parser_unexpected_value)
        PARSER_ASSERT_OR_ERROR(paramsLen == paramsBufferSize, parser_unexpected_number_items)

        CborParser parser;
        CborValue itParams;
        CHECK_CBOR_MAP_ERR(cbor_parser_init(tx->params, paramsLen, 0, &parser, &itParams))

        switch (itParams.type) {
            case CborArrayType: {
                size_t arrayLength = 0;
                CHECK_CBOR_MAP_ERR(cbor_value_get_array_length(&itParams, &arrayLength))
                PARSER_ASSERT_OR_ERROR(arrayLength < UINT8_MAX, parser_value_out_of_range)
                tx->numparams = arrayLength;
                break;
            }
            case CborMapType: {
                size_t mapLength = 0;
                CHECK_CBOR_MAP_ERR(cbor_value_get_map_length(&itParams, &mapLength))
                PARSER_ASSERT_OR_ERROR(mapLength < UINT8_MAX, parser_value_out_of_range)
                tx->numparams = mapLength;
                break;
            }
            case CborInvalidType:
            default:
                return parser_unexpected_type;
        }
    }
    tx->method = methodValue;

    return parser_ok;
}

parser_error_t _read(const parser_context_t *c, parser_tx_t *v) {
    CborValue it;
    INIT_CBOR_PARSER(c, it)
    PARSER_ASSERT_OR_ERROR(!cbor_value_at_end(&it), parser_unexpected_buffer_end)

    // It is an array
    PARSER_ASSERT_OR_ERROR(cbor_value_is_array(&it), parser_unexpected_type)
    size_t arraySize;
    CHECK_CBOR_MAP_ERR(cbor_value_get_array_length(&it, &arraySize))

    // Depends if we have params or not
    PARSER_ASSERT_OR_ERROR(arraySize == 10 || arraySize == 9, parser_unexpected_number_items)

    CborValue arrayContainer;
    PARSER_ASSERT_OR_ERROR(cbor_value_is_container(&it), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_enter_container(&it, &arrayContainer))

    // "version" field
    PARSER_ASSERT_OR_ERROR(cbor_value_is_integer(&arrayContainer), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_get_int64_checked(&arrayContainer, &v->version))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    if (v->version != COIN_SUPPORTED_TX_VERSION) {
        return parser_unexpected_tx_version;
    }

    // "to" field
    CHECK_PARSER_ERR(readAddress(&v->to, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "from" field
    CHECK_PARSER_ERR(readAddress(&v->from, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "nonce" field
    PARSER_ASSERT_OR_ERROR(cbor_value_is_unsigned_integer(&arrayContainer), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_get_uint64(&arrayContainer, &v->nonce))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "value" field
    CHECK_PARSER_ERR(readBigInt(&v->value, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "gasLimit" field
    PARSER_ASSERT_OR_ERROR(cbor_value_is_integer(&arrayContainer), parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_get_int64_checked(&arrayContainer, &v->gaslimit))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "gasFeeCap" field
    CHECK_PARSER_ERR(readBigInt(&v->gasfeecap, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "gasPremium" field
    CHECK_PARSER_ERR(readBigInt(&v->gaspremium, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    // "method" field
    CHECK_PARSER_ERR(readMethod(v, &arrayContainer))
    PARSER_ASSERT_OR_ERROR(arrayContainer.type != CborInvalidType, parser_unexpected_type)
    CHECK_CBOR_MAP_ERR(cbor_value_advance(&arrayContainer))

    CHECK_CBOR_MAP_ERR(cbor_value_leave_container(&it, &arrayContainer))

    // End of buffer does not match end of parsed data
    PARSER_ASSERT_OR_ERROR(it.ptr == c->buffer + c->bufferLen, parser_cbor_unexpected_EOF)

    return parser_ok;
}

parser_error_t _validateTx(const parser_context_t *c, const parser_tx_t *v) {
    (void) c;
    (void) v;
    // Note: This is place holder for transaction level checks that the project may require before accepting
    // the parsed values. the parser already validates input
    // This function is called by parser_validate, where additional checks are made (formatting, UI/UX, etc.(
    return parser_ok;
}

uint8_t _getNumItems(const parser_context_t *c, const parser_tx_t *v) {
    UNUSED(c);
    uint8_t itemCount = 8;

    return itemCount + v->numparams;
}
