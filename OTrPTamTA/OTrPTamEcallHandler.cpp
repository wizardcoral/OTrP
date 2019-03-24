/* Copyright (c) Microsoft Corporation.  All Rights Reserved. */
#include <openenclave/enclave.h>
#include "OTrPTam_t.h"

#include <stdbool.h>
#include <string.h>
extern "C" {
#include "jansson.h"
#include "joseinit.h"
#include "jose/jwe.h"
#include "jose/jwk.h"
#include "jose/jws.h"
#include "jose/b64.h"
#include "jose/openssl.h"
#include "../OTrPCommonTALib/common.h"
    char* strdup(const char* str);
};
#include "../jansson/JsonAuto.h"
#include "openssl/x509.h"
#include "openssl/evp.h"

#define UNIQUE_ID_LEN 16

/* Try to constrict a globally unique value. */
json_t* GetNewGloballyUniqueID(void)
{
    /* Create a random 16-byte value. */
    unsigned char value[UNIQUE_ID_LEN];
    oe_result_t result = oe_random(value, UNIQUE_ID_LEN);
    if (result != OE_OK) {
        return nullptr;
    }

    /* Base64-encode it into a string. */
    return jose_b64_enc(value, sizeof(value));
}

/* Construct a unique request ID.  The OTrP spec does not say what
 * the scope of uniqueness needs to be, but we currently try to use
 * globally unique value.
 */
json_t* GetNewRequestID(void)
{
    return GetNewGloballyUniqueID();
}

/* Construct a unique transaction ID.  The OTrP spec does not say what
 * the scope of uniqueness needs to be, but we currently try to use
 * a globally unique value.
 */
json_t* GetNewTransactionID(void)
{
    return GetNewGloballyUniqueID();
}

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

const unsigned char* g_TamDerCertificate = nullptr;
size_t g_TamDerCertificateSize = 0;

const unsigned char* GetTamDerCertificate(size_t *pCertLen)
{
    if (g_TamDerCertificate == nullptr) {
        // Construct a self-signed DER certificate based on the JWK.

        // First get the RSA key.
        json_t* jwk = GetTamEncryptionKey();
        g_TamDerCertificate = GetDerCertificate(jwk, &g_TamDerCertificateSize);
    }

    *pCertLen = g_TamDerCertificateSize;
    return g_TamDerCertificate;
}

/* Compose a GetDeviceStateTBSRequest message. */
const char* ComposeGetDeviceStateTBSRequest(void)
{
    JsonAuto object(json_object(), true);
    if (object == nullptr) {
        return nullptr;
    }
    JsonAuto request = object.AddObjectToObject("GetDeviceStateTBSRequest");
    if (request == nullptr) {
        return nullptr;
    }
    if (request.AddStringToObject("ver", "1.0") == nullptr) {
        return nullptr;
    }
    if (request.AddObjectToObject("rid", GetNewRequestID()) == nullptr) {
        return nullptr;
    }
    if (request.AddObjectToObject("tid", GetNewTransactionID()) == nullptr) {
        return nullptr;
    }
    JsonAuto ocspdat = request.AddArrayToObject("ocspdat");
    if (ocspdat == nullptr) {
        return nullptr;
    }
    /* TODO: Fill in list of OCSP stapling data. */

    /* supportedsigalgs is optional, so omit for now. */

    /* Convert to message buffer. */
    const char* message = json_dumps(object, 0);
    if (message == nullptr) {
        return nullptr;
    }
    return strdup(message);
}

const char* ComposeGetDeviceStateRequest(void)
{
    json_t* jwk = GetTamSigningKey();

    /* Compose a raw GetDeviceState request to be signed. */
    const char* tbsRequest = ComposeGetDeviceStateTBSRequest();
    if (tbsRequest == nullptr) {
        return nullptr;
    }
#ifdef _DEBUG
    printf("Sending TBS: %s\n", tbsRequest);
#endif

    /* Base64 encode it. */
    size_t len = strlen(tbsRequest);
    json_t* b64Request = jose_b64_enc(tbsRequest, len);
    free((void*)tbsRequest);
    if (b64Request == nullptr) {
        return nullptr;
    }

    /* Create the signed message. */
    JsonAuto jws(json_pack("{s:o}", "payload", b64Request), true);
    if ((json_t*)jws == nullptr) {
        return nullptr;
    }

    JsonAuto sig(json_object(), true);
    JsonAuto header = sig.AddObjectToObject("header");
    if ((json_t*)header == nullptr) {
        return nullptr;
    }
    JsonAuto x5c(json_array(), true);
    if (json_object_set_new(header, "x5c", x5c) < 0) {
        return nullptr;
    }

    // Get TAM DER cert.
    size_t certLen;
    const unsigned char* cert = GetTamDerCertificate(&certLen);
    json_t* certJson = jose_b64_enc(cert, certLen);
    if (json_array_append(x5c, certJson) < 0) {
        return nullptr;
    }

    bool ok = jose_jws_sig(
        nullptr,    // Configuration context (optional)
        jws,     // The JWE object
        sig,     // The JWE recipient object(s) or nullptr
        jwk);   // The JWK(s) or JWKSet used for wrapping.
    if (!ok) {
        return nullptr;
    }

    /* Create the final GetDeviceStateRequest message. */
    JsonAuto object(json_object(), true);
    if ((json_t*)object == nullptr) {
        return nullptr;
    }
    if (object.AddObjectToObject("GetDeviceStateRequest", jws) == nullptr) {
        return nullptr;
    }

    /* Serialize it to a single string. */
    const char* message = json_dumps(object, 0);
    return message;
}

/* Handle a new incoming connection from a device. */
int ecall_ProcessOTrPConnect(void* sessionHandle)
{
    printf("Received client connection\n");

    const char* message = ComposeGetDeviceStateRequest();
    if (message == nullptr) {
        return 1; /* Error */
    }

    printf("Sending GetDeviceStateRequest...\n");

    int err = 0;
    oe_result_t result = ocall_QueueOutboundOTrPMessage(&err, sessionHandle, message);
    free((void*)message);
    if (result != OE_OK) {
        return result;
    }

    return err;
}

/* Handle a GetDeviceTEEStateResponse from an OTrP Agent. */
int OTrPHandleGetDeviceTEEStateResponse(void* sessionHandle, const json_t* messageObject)
{
    if (!json_is_object(messageObject)) {
        return 1; /* Error */
    }

    /* Get the JWS signed object. */
    json_t* jws = json_object_get(messageObject, "GetDeviceTEEStateResponse");
    if (jws == nullptr) {
        return 1; /* Error */
    }
#ifdef _DEBUG
    const char* message = json_dumps(jws, 0);
    free((char*)message);
#endif

    /* Parse the JSON "payload" property and decrypt the JSON element
     * "edsi".  The decrypted message contains the TEE signer
     * certificate.
     */

    char* payload = DecodeJWS(jws, nullptr);
    if (payload == nullptr) {
        return 1; /* Error */
    }

    JsonAuto object(json_loads(payload, 0, nullptr));
    free(payload);
    if ((json_t*)object == nullptr) {
        return 1; /* Error */
    }

    json_t* tbs = json_object_get(object, "GetDeviceTEEStateTBSResponse");
    if (tbs == nullptr || !json_is_object(tbs)) {
        return 1; /* Error */
    }

    json_t* edsi = json_object_get(tbs, "edsi");
    if (edsi == nullptr || !json_is_object(edsi)) {
        return 1; /* Error */
    }

    /* Decrypt the edsi. */
    json_t* jwkEncryption = GetTamEncryptionKey();
    size_t len = 0;
    void* dsibuffer = jose_jwe_dec(nullptr, edsi, nullptr, jwkEncryption, &len);

    /* Copy it to a string buffer. */
    char* dsistr = (char*)malloc(len + 1);
    if (dsistr == nullptr) {
        return 1; /* Error */
    }
    memcpy(dsistr, dsibuffer, len);
    dsistr[len] = 0;

    /* Deserialize it into a JSON object. */
    json_error_t error;
    JsonAuto dsiWrapper(json_loads(dsistr, 0, &error), true);
    free(dsistr);
    if ((json_t*)dsiWrapper == nullptr) {
        return 1; /* Error */
    }

    // Extract the signer certificate from dsi.tee.cert.
    json_t* dsi = json_object_get(dsiWrapper, "dsi");
    if (dsi == nullptr || !json_is_object(dsi)) {
        return 1;
    }
    json_t* tee = json_object_get(dsi, "tee");
    if (tee == nullptr || !json_is_object(tee)) {
        return 1;
    }
    json_t* cert = json_object_get(tee, "cert");
    if (cert == nullptr || !json_is_string(cert)) {
        return 1;
    }
    size_t certSize = jose_b64_dec(cert, nullptr, 0);
    void* certBuffer = malloc(certSize);
    if (certBuffer == nullptr) {
        return 1;
    }
    if (jose_b64_dec(cert, certBuffer, certSize) != certSize) {
        free(certBuffer);
        return 1;
    }

    // Create a JWK from the device agent's cert.

    // Read DER buffer into X509 structure per https://stackoverflow.com/questions/6689584/how-to-convert-the-certificate-string-into-x509-structure
    // since the openssl version we currently use does not have d2i_x509() directly.
    BIO* bio = BIO_new(BIO_s_mem());
    BIO_write(bio, certBuffer, certSize);
    X509* x509 = d2i_X509_bio(bio, nullptr);
    free(certBuffer);
    BIO_free(bio);

    EVP_PKEY *pkey = X509_get_pubkey(x509);
    RSA* rsa = EVP_PKEY_get1_RSA(pkey);
    JsonAuto jwkTemp(jose_openssl_jwk_from_RSA(nullptr, rsa), true);
    JsonAuto jwkAgent(CopyToJweKey(jwkTemp, "RS256"), true);
    EVP_PKEY_free(pkey);

    // Verify the signature using the signer certificate.
    bool ok = jose_jws_ver(nullptr, jws, nullptr, jwkAgent, false);
    if (!ok) {
        return 1;
    }

    // Extract TEE information and check it against TEE acceptance policy.
    // TODO

    // Extract the TFW signed element, and check the signer and data
    // integration against its TFW policy.
    // TODO

    // Check the SD list and TA list and prepare for a subsequent command
    // such as "CreateSD" if it needs to have a new SD for an SP.
    // TODO
    json_t* tarequestlist = json_object_get(tee, "tarequestlist");
    if (tarequestlist != nullptr) {
        if (!json_is_array(tarequestlist)) {
            return 1;
        }
        size_t index;
        json_t* ta;
        const char* taid = nullptr;
        json_array_foreach(tarequestlist, index, ta) {
            json_t* jtaid = json_object_get(messageObject, "taid");
            taid = json_string_value(jtaid);

            // TODO: decide whether taid should be installed.
            bool okToInstall = true;
            if (!okToInstall) {
                continue;
            }

            // TODO: send an InstallTARequest message.
            break;
        }
    }

    return 0; /* no error */
}

/* Handle a GetDeviceStateResponse from an OTrP Agent. */
int OTrPHandleGetDeviceStateResponse(void* sessionHandle, const json_t* messageObject)
{
    if (!json_is_array(messageObject)) {
        return 1; /* Error */
    }

    // Parse to get list of GetDeviceTEEStateResponse JSON objects.
    size_t index;
    json_t* value;
    json_array_foreach(messageObject, index, value) {
        int err = OTrPHandleGetDeviceTEEStateResponse(sessionHandle, value);
        if (err != 0) {
            return err;
        }
    }

    return 0; /* no error */
}

/* Handle an incoming message from an OTrP Agent. */
/* Returns 0 on success, or non-zero if error. */
int OTrPHandleMessage(void* sessionHandle, const char* key, const json_t* messageObject)
{
    if (strcmp(key, "GetDeviceStateResponse") == 0) {
        return OTrPHandleGetDeviceStateResponse(sessionHandle, messageObject);
    }

    /* Unrecognized message. */
    return 1;
}