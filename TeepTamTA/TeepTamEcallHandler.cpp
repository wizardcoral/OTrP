/* Copyright (c) Microsoft Corporation.  All Rights Reserved. */
#include <openenclave/enclave.h>
#include "TeepTam_t.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "jansson.h"
#include "joseinit.h"
#include "jose/jwe.h"
#include "jose/jwk.h"
#include "jose/jws.h"
#include "jose/b64.h"
#include "jose/openssl.h"
#include "../TeepCommonTALib/common.h"
#include "../TeepCommonTALib/otrp.h"
#include "../TeepCommonTALib/teep_protocol.h"
};
#include "../jansson/JsonAuto.h"
#include "qcbor/qcbor_decode.h"
#include "qcbor/qcbor_encode.h"
#include "openssl/x509.h"
#include "openssl/evp.h"
#include "Manifest.h"
#include "OTrPTamEcallHandler.h"
#include "TeepTamEcallHandler.h"
#include "RequestedComponentInfo.h"

#ifdef TEEP_ENABLE_JSON
JsonAuto g_TamSigningKey;

json_t* GetTamSigningKey()
{
    if ((json_t*)g_TamSigningKey == nullptr) {
        g_TamSigningKey = CreateNewJwkRS256();
    }
    return (json_t*)g_TamSigningKey;
}

JsonAuto g_TamEncryptionKey;

json_t* GetTamEncryptionKey()
{
    if ((json_t*)g_TamEncryptionKey == nullptr) {
        g_TamEncryptionKey = CopyToJweKey(GetTamSigningKey(), "RSA1_5");
    }
    return g_TamEncryptionKey;
}
#endif

const unsigned char* g_TamDerCertificate = nullptr;
size_t g_TamDerCertificateSize = 0;

const unsigned char* GetTamDerCertificate(size_t *pCertLen)
{
    if (g_TamDerCertificate == nullptr) {
        // Construct a self-signed DER certificate based on the JWK.

        // First get the RSA key.
#ifdef TEEP_ENABLE_JSON
        json_t* jwk = GetTamEncryptionKey();
        g_TamDerCertificate = GetDerCertificate(jwk, &g_TamDerCertificateSize);
#else
        // TODO
        return nullptr;
#endif
    }

    *pCertLen = g_TamDerCertificateSize;
    return g_TamDerCertificate;
}

json_t* GetNewGloballyUniqueID(void);

/* Construct a unique request token.  The TEEP spec does not say what
 * the scope of uniqueness needs to be, but we currently try to use
 * globally unique value.
 */
json_t* GetNewToken(void)
{
    return GetNewGloballyUniqueID();
}

// Compose a JSON Query Request message to be signed.
const char* TeepComposeJsonQueryRequestTBS(void)
{
    JsonAuto request(json_object(), true);
    if (request == nullptr) {
        return nullptr;
    }
    if (request.AddIntegerToObject("TYPE", TEEP_MESSAGE_QUERY_REQUEST) == nullptr) {
        return nullptr;
    }

    if (request.AddObjectToObject("TOKEN", GetNewToken()) == nullptr) {
        return nullptr;
    }

    JsonAuto dataItems = request.AddArrayToObject("REQUEST");
    if (dataItems == nullptr) {
        return nullptr;
    }
    if (dataItems.AddIntegerToArray(TEEP_TRUSTED_COMPONENTS) == nullptr) {
        return nullptr;
    }

    // Convert to message buffer.
    const char* message = json_dumps(request, 0);
    return message;
}

int TeepComposeCborQueryRequestTBS(UsefulBufC* encoded)
{
    QCBOREncodeContext context;
    UsefulBuf buffer = UsefulBuf_Unconst(*encoded);
    QCBOREncode_Init(&context, buffer);

    QCBOREncode_OpenArray(&context);
    {
        // Add TYPE.
        QCBOREncode_AddInt64(&context, TEEP_MESSAGE_QUERY_REQUEST);

        // Create a random 16-byte token.
        unsigned char token[UUID_LENGTH];
        oe_result_t result = oe_random(token, sizeof(token));
        if (result != OE_OK) {
            return result;
        }
        QCBOREncode_AddBytes(&context, UsefulBuf_FROM_BYTE_ARRAY_LITERAL(token));

        // Draft -03 implies we have to store the token for validation
        // upon receiving a QueryResponse, but that adversely affects
        // scalability, opens the protocol to DOS attacks similar to SYN attacks,
        // and forces the extra round trip.  See
        // https://github.com/ietf-teep/teep-protocol/issues/40 for discussion.
        // As such, we currently don't implement such a check in the hopes
        // that the draft will remove the check in the future.  But we have
        // to include a token anyway for interoperability.

        QCBOREncode_OpenMap(&context);
        {
            // Insert optional items here once labels are defined.
        }
        QCBOREncode_CloseMap(&context);

        QCBOREncode_AddInt64(&context, TEEP_TRUSTED_COMPONENTS);
    }
    QCBOREncode_CloseArray(&context);

    QCBORError err = QCBOREncode_Finish(&context, encoded);
    return err;
}

const char* TeepComposeJsonQueryRequest()
{
    // Compose a raw QueryRequest message to be signed.
    const char* tbsRequest = TeepComposeJsonQueryRequestTBS();
    if (tbsRequest == nullptr) {
        return nullptr;
    }
#ifdef _DEBUG
    printf("Sending TBS: %s\n", tbsRequest);
#endif
    return tbsRequest;
}

int TeepComposeCborQueryRequest(UsefulBufC* bufferToSend)
{
    /* Compose a raw QueryRequest message to be signed. */
    return TeepComposeCborQueryRequestTBS(bufferToSend);
}

/* Handle a new incoming connection from a device. */
int TeepProcessConnect(void* sessionHandle, const char* mediaType)
{
    printf("Received client connection\n");

    int err = 0;
    UsefulBufC encoded;
#ifdef TEEP_ENABLE_JSON
    if (strcmp(mediaType, TEEP_JSON_MEDIA_TYPE) == 0) {
        const char* message = TeepComposeJsonQueryRequest();
        if (message == nullptr) {
            return 1; /* Error */
        }
        encoded.ptr = message;
        encoded.len = strlen(message);
    } else
#endif
    {
        int maxBufferLength = 4096;
        char* buffer = (char*)malloc(maxBufferLength);
        if (buffer == nullptr) {
            return 1; /* Error */
        }
        encoded.ptr = buffer;
        encoded.len = maxBufferLength;

        err = TeepComposeCborQueryRequest(&encoded);
        if (err != 0) {
            return err;
        }

        if (encoded.len == 0) {
            return 1; /* Error */
        }

        printf("Sending CBOR message: ");
        HexPrintBuffer(encoded.ptr, encoded.len);
    }
    
    printf("Sending QueryRequest...\n");

    oe_result_t result = ocall_QueueOutboundTeepMessage(&err, sessionHandle, mediaType, (const char*)encoded.ptr, encoded.len);
    free((void*)encoded.ptr);
    if (result != OE_OK) {
        return result;
    }

    return err;
}

int ecall_ProcessTeepConnect(void* sessionHandle, const char* acceptMediaType)
{
#ifdef ENABLE_OTRP
    if (strncmp(acceptMediaType, OTRP_JSON_MEDIA_TYPE, strlen(OTRP_JSON_MEDIA_TYPE)) == 0) {
        return OTrPProcessConnect(sessionHandle);
    } else
#endif
    if ((strncmp(acceptMediaType, TEEP_CBOR_MEDIA_TYPE, strlen(TEEP_CBOR_MEDIA_TYPE)) == 0)
#ifdef TEEP_ENABLE_JSON
            || (strncmp(acceptMediaType, TEEP_JSON_MEDIA_TYPE, strlen(TEEP_JSON_MEDIA_TYPE)) == 0)
#endif
        ) {
        return TeepProcessConnect(sessionHandle, acceptMediaType);
    } else {
        return 1;
    }
}

// Get the BASE64-encoded SHA256 hash value of the buffer.
json_t* GetSha256Hash(void* buffer, int len)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, buffer, len);
    SHA256_Final(hash, &sha256);

    return jose_b64_enc(hash, sizeof(hash));
}

/* Handle an incoming message from a TEEP Agent. */
/* Returns 0 on success, or non-zero if error. */
int TeepHandleJsonMessage(void* sessionHandle, const char* message, unsigned int messageLength)
{
    (void)sessionHandle; // Unused.
    (void)message; // Unused.
    (void)messageLength; // Unused.

    /* Unrecognized message. */
    return 1;
}

// Returns 0 on success, non-zero on error.
int TeepComposeCborInstallTBS(UsefulBufC* encoded, RequestedComponentInfo* requestedComponentList)
{
    encoded->ptr = nullptr;
    encoded->len = 0;

    int maxBufferLength = 4096;
    char* rawBuffer = (char*)malloc(maxBufferLength);
    if (rawBuffer == nullptr) {
        return 1; /* Error */
    }
    encoded->ptr = rawBuffer;
    encoded->len = maxBufferLength;

    QCBOREncodeContext context;
    UsefulBuf buffer = UsefulBuf_Unconst(*encoded);
    QCBOREncode_Init(&context, buffer);

    QCBOREncode_OpenArray(&context);
    {
        // Add TYPE.
        QCBOREncode_AddInt64(&context, TEEP_MESSAGE_INSTALL);

        /* Create a random 16-byte token. */
        unsigned char token[UUID_LENGTH];
        oe_result_t result = oe_random(token, sizeof(token));
        if (result != OE_OK) {
            return result;
        }
        QCBOREncode_AddBytes(&context, UsefulBuf_FROM_BYTE_ARRAY_LITERAL(token));

        QCBOREncode_OpenMap(&context);
        {
            QCBOREncode_OpenArrayInMapN(&context, TEEP_LABEL_MANIFEST_LIST);
            {
                // Add SUIT manifest for any requested components that we decide to install.
                for (RequestedComponentInfo* rci = requestedComponentList; rci != nullptr; rci = rci->Next) {
                    // TODO: make a decision whether to install it or not.  For now, we go ahead.
                    UsefulBufC manifest;
                    manifest.ptr = Manifest::GetManifest(&rci->ComponentId, &manifest.len);
                    if (manifest.ptr != nullptr) {
                        QCBOREncode_AddEncoded(&context, manifest);
                    }
                }
            }
            QCBOREncode_CloseArray(&context);
        }
        QCBOREncode_CloseMap(&context);
    }
    QCBOREncode_CloseArray(&context);

    QCBORError err = QCBOREncode_Finish(&context, encoded);
    return err;
}

// Returns 0 on success, non-zero on error.
int TeepComposeCborInstall(UsefulBufC* install, RequestedComponentInfo* requestedComponentList)
{
    /* Compose a raw Install message to be signed. */
    return TeepComposeCborInstallTBS(install, requestedComponentList);
}

// Returns 0 on success, non-zero on error.
int TeepHandleCborQueryResponse(void* sessionHandle, QCBORDecodeContext* context)
{
    (void)sessionHandle;
    (void)context;

    printf("TeepHandleCborQueryResponse\n");

    /* 1.  Validate COSE message signing.  If it doesn't pass, an error message is returned. */
    /* ... TODO ... */

    /* 2.  Validate that the certificate is chained to a trusted
     *     CA that the TAM embeds as its trust anchor.
     */
    /* ... TODO ... */

    QCBORItem item;
    QCBORDecode_GetNext(context, &item);
    if (item.uDataType != QCBOR_TYPE_BYTE_STRING) {
        printf("Invalid token type %d\n", item.uDataType);
        return 1; // Invalid message.
    }

    /* As discussed above in comments in TeepComposeCborQueryRequestTBS(),
     * draft -03 requires us to validate that the token matches what was
     * sent in the QueryRequest, but that causes performance problems and
     * opens us to certain DOS attacks, without any obvious benefit. As such,
     * we skip this check in the hopes that the spec will be updated to
     * remove the check.
     */

    // Parse the options map.
    QCBORDecode_GetNext(context, &item);
    if (item.uDataType != QCBOR_TYPE_MAP) {
        printf("Invalid options type %d\n", item.uDataType);
        return 1; // Invalid message.
    }
    RequestedComponentInfo requestedComponentList(nullptr);
    uint16_t mapEntryCount = item.val.uCount;
    for (int mapEntryIndex = 0; mapEntryIndex < mapEntryCount; mapEntryIndex++) {
        QCBORDecode_GetNext(context, &item);
        teep_label_t label = (teep_label_t)item.label.int64;
        switch (label) {
        case TEEP_LABEL_SELECTED_VERSION:
            if (item.val.uint64 != 0) {
                printf("Unrecognized protocol version %lld\n", item.val.uint64);
                return 1; /* invalid message */
            }
            break;
        case TEEP_LABEL_SELECTED_CIPHER_SUITE:
            if ((item.val.uint64 != TEEP_CIPHERSUITE_ES256) &&
                (item.val.uint64 != TEEP_CIPHERSUITE_EDDSA)) {
                printf("Unrecognized cipher suite %lld\n", item.val.uint64);
                return 1; /* invalid ciphersuite */
            }
            break;
        case TEEP_LABEL_REQUESTED_TC_LIST:
        {
            if (item.uDataType != QCBOR_TYPE_ARRAY) {
                printf("Invalid requested-tc-list type %d\n", item.uDataType);
                return 1; // Invalid message.
            }
            uint16_t arrayEntryCount = item.val.uCount;
            for (int arrayEntryIndex = 0; arrayEntryIndex < arrayEntryCount; arrayEntryIndex++) {
                QCBORDecode_GetNext(context, &item);
                if (item.uDataType != QCBOR_TYPE_MAP) {
                    printf("Invalid requested-tc-info type %d\n", item.uDataType);
                    return 1; // Invalid message.
                }
                uint16_t tcInfoParameterCount = item.val.uCount;
                RequestedComponentInfo* currentRci = nullptr;
                for (int tcInfoParameterIndex = 0; tcInfoParameterIndex < tcInfoParameterCount; tcInfoParameterIndex++) {
                    QCBORDecode_GetNext(context, &item);
                    teep_label_t label = (teep_label_t)item.label.int64;
                    switch (label) {
                    case TEEP_LABEL_COMPONENT_ID:
                    {
                        if (item.uDataType != QCBOR_TYPE_BYTE_STRING) {
                            printf("Invalid component-id type %d\n", item.uDataType);
                            return 1;
                        }
                        if (currentRci != nullptr) {
                            // Duplicate.
                            return 1;
                        }
                        currentRci = new RequestedComponentInfo(&item.val.string);
                        currentRci->Next = requestedComponentList.Next;
                        requestedComponentList.Next = currentRci;
                        break;
                    }
                    case TEEP_LABEL_TC_MANIFEST_SEQUENCE_NUMBER:
                        if (item.uDataType != QCBOR_TYPE_UINT64) {
                            return 1; /* invalid message */
                        }
                        currentRci->ManifestSequenceNumber = item.val.uint64;
                        break;
                    case TEEP_LABEL_HAVE_BINARY:
                        if (item.uDataType != QCBOR_TYPE_UINT64) {
                            return 1; /* invalid message */
                        }
                        currentRci->HaveBinary = (item.val.uint64 != 0);
                        break;
                    default:
                        printf("Unrecognized option label %d\n", label);
                        return 1; /* invalid message */
                    }
                }
            }
            break;
        }
        case TEEP_LABEL_EVIDENCE_FORMAT:
        case TEEP_LABEL_EVIDENCE:
        case TEEP_LABEL_TC_LIST:
        case TEEP_LABEL_UNNEEDED_TC_LIST:
            printf("Ignoring unimplemented option label %d\n", label);
            break;
        default:
            printf("Unrecognized option label %d\n", label);
            return 1; /* invalid message */
        }
    }

    // 3. Compose an Install message.
    if (requestedComponentList.Next != nullptr) {
        UsefulBufC install;
        int err = TeepComposeCborInstall(&install, requestedComponentList.Next);
        if (err != 0) {
            return err;
        }
        if (install.len == 0) {
            return 1; // Error.
        }

        printf("Sending CBOR message: ");
        HexPrintBuffer(install.ptr, install.len);

        printf("Sending Install message...\n");

        oe_result_t result = ocall_QueueOutboundTeepMessage(&err, sessionHandle, TEEP_CBOR_MEDIA_TYPE, (const char*)install.ptr, install.len);
        free((void*)install.ptr);
        if (result != OE_OK) {
            return result;
        }
        return err;
    }

    return 0;
}

/* Handle an incoming message from a TEEP Agent. */
/* Returns 0 on success, or non-zero if error. */
int TeepHandleCborMessage(void* sessionHandle, const char* message, unsigned int messageLength)
{
    QCBORDecodeContext context;
    QCBORItem item;
    UsefulBufC encoded;
    encoded.ptr = message;
    encoded.len = messageLength;

    printf("Received CBOR message: ");
    HexPrintBuffer(encoded.ptr, encoded.len);

    QCBORDecode_Init(&context, encoded, QCBOR_DECODE_MODE_NORMAL);

    QCBORDecode_GetNext(&context, &item);
    if (item.uDataType != QCBOR_TYPE_ARRAY) {
        printf("Invalid TYPE type %d\n", item.uDataType);
        return 1; // Invalid message.
    }

    QCBORDecode_GetNext(&context, &item);
    if (item.uDataType != QCBOR_TYPE_INT64) {
        printf("Invalid TYPE type %d\n", item.uDataType);
        return 1; // Invalid message.
    }

    teep_message_type_t messageType = (teep_message_type_t)item.val.uint64;
    printf("Received CBOR TEEP message type=%d\n", messageType);
    switch (messageType) {
    case TEEP_MESSAGE_QUERY_RESPONSE:
        if (TeepHandleCborQueryResponse(sessionHandle, &context) != 0) {
            return 1;
        }
        break;
    default:
        return 1; // Unknown message type.
        break;
    }

    QCBORError err = QCBORDecode_Finish(&context);
    return err;
}
