#include "apple_conduit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

static int recv_exact(SOCKET s, char *buf, int len) {
    int total = 0;
    while (total < len) {
        int r = recv(s, buf + total, len - total, 0);
        if (r <= 0) return total;
        total += r;
    }
    return total;
}

static SOCKET usbmux_connect_port(int devId, unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(27015);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) != 0) return INVALID_SOCKET;

    char connPlist[512];
    sprintf(connPlist,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>MessageType</key><string>Connect</string>\n"
        "<key>ClientVersionString</key><string>EyeChoons</string>\n"
        "<key>ProgName</key><string>EyeChoons</string>\n"
        "<key>DeviceID</key><integer>%d</integer>\n"
        "<key>PortNumber</key><integer>%d</integer>\n"
        "</dict></plist>", devId, htons(port));
    unsigned int plLen = (unsigned int)strlen(connPlist);
    unsigned int hdr[4] = { 16 + plLen, 1, 8, 2 };
    send(s, (char*)hdr, 16, 0);
    send(s, connPlist, plLen, 0);

    unsigned int rHdr[4];
    if (recv_exact(s, (char*)rHdr, 16) != 16) { closesocket(s); return INVALID_SOCKET; }
    char *resp = (char*)malloc(rHdr[0] - 16 + 1);
    recv_exact(s, resp, rHdr[0] - 16);
    resp[rHdr[0] - 16] = 0;
    int num = -1;
    char *pNum = strstr(resp, "<key>Number</key>");
    if (pNum) {
        char *pInt = strstr(pNum, "<integer>");
        if (pInt) sscanf(pInt, "<integer>%d</integer>", &num);
    }
    free(resp);
    if (num != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static void send_lockdown_plist(SOCKET s, const char *xml) {
    unsigned int len = (unsigned int)strlen(xml);
    unsigned int beLen = htonl(len);
    send(s, (char*)&beLen, 4, 0);
    send(s, xml, len, 0);
}

static char *recv_lockdown_plist(SOCKET s) {
    unsigned int beLen = 0;
    if (recv_exact(s, (char*)&beLen, 4) != 4) return NULL;
    unsigned int len = ntohl(beLen);
    char *buf = (char*)malloc(len + 1);
    if (recv_exact(s, buf, len) != (int)len) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

int Apple_FindDevice(int *outDevId, char *outUdid, int maxUdid) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(27015);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) != 0) return 0;

    DWORD tv = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    const char *listenPlist = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>MessageType</key><string>Listen</string>\n"
        "<key>ClientVersionString</key><string>EyeChoons</string>\n"
        "<key>ProgName</key><string>EyeChoons</string>\n"
        "</dict></plist>";
    unsigned int plLen = (unsigned int)strlen(listenPlist);
    unsigned int hdr[4] = { 16 + plLen, 1, 8, 1 };
    send(s, (char*)hdr, 16, 0);
    send(s, listenPlist, plLen, 0);

    unsigned int rHdr[4];
    if (recv_exact(s, (char*)rHdr, 16) != 16) { closesocket(s); return 0; }
    char *dump = (char*)malloc(rHdr[0] - 16);
    recv_exact(s, dump, rHdr[0] - 16);
    free(dump);

    if (recv_exact(s, (char*)rHdr, 16) != 16) { closesocket(s); return 0; }
    char *evt = (char*)malloc(rHdr[0] - 16 + 1);
    recv_exact(s, evt, rHdr[0] - 16);
    evt[rHdr[0] - 16] = 0;

    int devId = 0;
    char *pDev = strstr(evt, "<key>DeviceID</key>");
    if (pDev) {
        char *pInt = strstr(pDev, "<integer>");
        if (pInt) sscanf(pInt, "<integer>%d</integer>", &devId);
    }
    if (outUdid && maxUdid > 0) {
        outUdid[0] = 0;
        char *pSer = strstr(evt, "<key>SerialNumber</key>");
        if (pSer) {
            char *pStr = strstr(pSer, "<string>");
            if (pStr) sscanf(pStr, "<string>%63[^<]</string>", outUdid);
        }
    }
    free(evt);
    closesocket(s);
    if (outDevId) *outDevId = devId;
    return (devId > 0);
}

static char *tls_request_plist(AppleSession *sess, const char *reqXml) {
    SecPkgContext_StreamSizes sizes;
    QueryContextAttributesA(&sess->hContext, SECPKG_ATTR_STREAM_SIZES, &sizes);

    unsigned int reqLen = (unsigned int)strlen(reqXml);
    unsigned int beLen = htonl(reqLen);
    int plainLen = 4 + reqLen;

    char *plainBuf = (char*)malloc(plainLen);
    memcpy(plainBuf, &beLen, 4);
    memcpy(plainBuf + 4, reqXml, reqLen);

    char *encBuf = (char*)malloc(sizes.cbHeader + plainLen + sizes.cbTrailer);
    memcpy(encBuf + sizes.cbHeader, plainBuf, plainLen);

    SecBuffer encryptBuf[4];
    encryptBuf[0].BufferType = SECBUFFER_STREAM_HEADER;
    encryptBuf[0].cbBuffer = sizes.cbHeader;
    encryptBuf[0].pvBuffer = encBuf;
    encryptBuf[1].BufferType = SECBUFFER_DATA;
    encryptBuf[1].cbBuffer = plainLen;
    encryptBuf[1].pvBuffer = encBuf + sizes.cbHeader;
    encryptBuf[2].BufferType = SECBUFFER_STREAM_TRAILER;
    encryptBuf[2].cbBuffer = sizes.cbTrailer;
    encryptBuf[2].pvBuffer = encBuf + sizes.cbHeader + plainLen;
    encryptBuf[3].BufferType = SECBUFFER_EMPTY;
    encryptBuf[3].cbBuffer = 0;
    encryptBuf[3].pvBuffer = NULL;

    SecBufferDesc encryptDesc = { SECBUFFER_VERSION, 4, encryptBuf };
    EncryptMessage(&sess->hContext, 0, &encryptDesc, 0);

    int totalEncLen = encryptBuf[0].cbBuffer + encryptBuf[1].cbBuffer + encryptBuf[2].cbBuffer;
    send(sess->sLock, encBuf, totalEncLen, 0);
    free(encBuf);
    free(plainBuf);

    char tlsRecvBuf[16384];
    int tlsRecvLen = 0;
    char decPayload[8192];
    int decPayloadLen = 0;

    while (decPayloadLen < 10) {
        if (tlsRecvLen == 0) {
            int r = recv(sess->sLock, tlsRecvBuf + tlsRecvLen, sizeof(tlsRecvBuf) - tlsRecvLen, 0);
            if (r <= 0) break;
            tlsRecvLen += r;
        }

        SecBuffer decBuf[4];
        decBuf[0].BufferType = SECBUFFER_DATA;
        decBuf[0].cbBuffer = tlsRecvLen;
        decBuf[0].pvBuffer = tlsRecvBuf;
        decBuf[1].BufferType = SECBUFFER_EMPTY;
        decBuf[2].BufferType = SECBUFFER_EMPTY;
        decBuf[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc decDesc = { SECBUFFER_VERSION, 4, decBuf };

        SECURITY_STATUS sc = DecryptMessage(&sess->hContext, &decDesc, 0, NULL);
        if (sc == SEC_E_INCOMPLETE_MESSAGE) {
            int r = recv(sess->sLock, tlsRecvBuf + tlsRecvLen, sizeof(tlsRecvBuf) - tlsRecvLen, 0);
            if (r <= 0) break;
            tlsRecvLen += r;
            continue;
        }
        if (sc != SEC_E_OK) break;

        int extraBytes = 0;
        for (int i = 0; i < 4; i++) {
            if (decBuf[i].BufferType == SECBUFFER_DATA && decBuf[i].cbBuffer > 0) {
                memcpy(decPayload + decPayloadLen, decBuf[i].pvBuffer, decBuf[i].cbBuffer);
                decPayloadLen += decBuf[i].cbBuffer;
            } else if (decBuf[i].BufferType == SECBUFFER_EXTRA) {
                extraBytes = decBuf[i].cbBuffer;
            }
        }

        if (extraBytes > 0) {
            memmove(tlsRecvBuf, tlsRecvBuf + (tlsRecvLen - extraBytes), extraBytes);
            tlsRecvLen = extraBytes;
        } else {
            tlsRecvLen = 0;
        }
    }

    decPayload[decPayloadLen] = 0;
    char *xmlStart = decPayload;
    if (decPayloadLen >= 4 && (unsigned char)decPayload[0] == 0) {
        xmlStart = decPayload + 4;
    }

    char *result = (char*)malloc(strlen(xmlStart) + 1);
    strcpy(result, xmlStart);
    return result;
}

int Apple_StartSession(AppleSession *sess, int devId, const char *udid) {
    if (!sess) return 0;
    memset(sess, 0, sizeof(AppleSession));
    sess->devId = devId;
    if (udid) strncpy(sess->udid, udid, sizeof(sess->udid) - 1);
    sess->sLock = INVALID_SOCKET;
    sess->sAfc = INVALID_SOCKET;
    sess->sNp = INVALID_SOCKET;
    sess->afcSeq = 1;

    char pairPath[MAX_PATH];
    sprintf(pairPath, "C:\\ProgramData\\Apple\\Lockdown\\%s.plist", sess->udid);
    FILE *fp = fopen(pairPath, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *pairBuf = (char*)malloc(sz + 1);
    fread(pairBuf, 1, sz, fp);
    pairBuf[sz] = 0;
    fclose(fp);

    char hostId[64] = {0};
    char *pHostId = strstr(pairBuf, "<key>HostID</key>");
    if (pHostId) sscanf(strstr(pHostId, "<string>"), "<string>%63[^<]</string>", hostId);

    char buid[64] = {0};
    char *pBuid = strstr(pairBuf, "<key>SystemBUID</key>");
    if (pBuid) sscanf(strstr(pBuid, "<string>"), "<string>%63[^<]</string>", buid);

    // Certificate
    char *pCert = strstr(pairBuf, "<key>HostCertificate</key>");
    if (!pCert) { free(pairBuf); return 0; }
    char *pCertData = strstr(pCert, "<data>") + 6;
    char *pCertEnd = strstr(pCertData, "</data>");
    *pCertEnd = 0;
    DWORD certPemLen = 0;
    CryptStringToBinaryA(pCertData, (DWORD)strlen(pCertData), CRYPT_STRING_BASE64, NULL, &certPemLen, NULL, NULL);
    BYTE *certPem = (BYTE*)malloc(certPemLen);
    CryptStringToBinaryA(pCertData, (DWORD)strlen(pCertData), CRYPT_STRING_BASE64, certPem, &certPemLen, NULL, NULL);
    *pCertEnd = '<';
    DWORD certDerLen = 0;
    CryptStringToBinaryA((char*)certPem, certPemLen, CRYPT_STRING_BASE64HEADER, NULL, &certDerLen, NULL, NULL);
    BYTE *certDer = (BYTE*)malloc(certDerLen);
    CryptStringToBinaryA((char*)certPem, certPemLen, CRYPT_STRING_BASE64HEADER, certDer, &certDerLen, NULL, NULL);
    PCCERT_CONTEXT pCertContext = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, certDer, certDerLen);

    // Key
    char *pKey = strstr(pairBuf, "<key>HostPrivateKey</key>");
    if (!pKey) { free(pairBuf); free(certPem); free(certDer); return 0; }
    char *pKeyData = strstr(pKey, "<data>") + 6;
    char *pKeyEnd = strstr(pKeyData, "</data>");
    *pKeyEnd = 0;
    DWORD keyPemLen = 0;
    CryptStringToBinaryA(pKeyData, (DWORD)strlen(pKeyData), CRYPT_STRING_BASE64, NULL, &keyPemLen, NULL, NULL);
    BYTE *keyPem = (BYTE*)malloc(keyPemLen);
    CryptStringToBinaryA(pKeyData, (DWORD)strlen(pKeyData), CRYPT_STRING_BASE64, keyPem, &keyPemLen, NULL, NULL);
    *pKeyEnd = '<';
    DWORD keyDerLen = 0;
    CryptStringToBinaryA((char*)keyPem, keyPemLen, CRYPT_STRING_BASE64HEADER, NULL, &keyDerLen, NULL, NULL);
    BYTE *keyDer = (BYTE*)malloc(keyDerLen);
    CryptStringToBinaryA((char*)keyPem, keyPemLen, CRYPT_STRING_BASE64HEADER, keyDer, &keyDerLen, NULL, NULL);

    PCRYPT_PRIVATE_KEY_INFO pKeyInfo = NULL;
    DWORD keyInfoLen = 0;
    BYTE *pRawRsa = NULL;
    DWORD rawRsaLen = 0;
    CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, keyDer, keyDerLen, CRYPT_DECODE_ALLOC_FLAG, NULL, &pKeyInfo, &keyInfoLen);
    CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, pKeyInfo->PrivateKey.pbData, pKeyInfo->PrivateKey.cbData, CRYPT_DECODE_ALLOC_FLAG, NULL, &pRawRsa, &rawRsaLen);

    HCRYPTPROV hProv = 0;
    CryptAcquireContextW(&hProv, L"EyeChoonsKeyContainer", MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    CryptAcquireContextW(&hProv, L"EyeChoonsKeyContainer", MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET);
    HCRYPTKEY hRsaKey = 0;
    CryptImportKey(hProv, pRawRsa, rawRsaLen, 0, CRYPT_EXPORTABLE, &hRsaKey);

    CRYPT_KEY_PROV_INFO kpi = {0};
    kpi.pwszContainerName = L"EyeChoonsKeyContainer";
    kpi.pwszProvName = MS_ENH_RSA_AES_PROV_W;
    kpi.dwProvType = PROV_RSA_AES;
    kpi.dwKeySpec = AT_KEYEXCHANGE;
    CertSetCertificateContextProperty(pCertContext, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi);

    SCHANNEL_CRED cred = {0};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.cCreds = 1;
    cred.paCred = &pCertContext;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;

    TimeStamp ts;
    SECURITY_STATUS sc = AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &sess->hCred, &ts);
    if (sc != SEC_E_OK) {
        free(pairBuf); free(certPem); free(certDer); free(keyPem); free(keyDer);
        return 0;
    }

    // Connect to lockdownd
    sess->sLock = usbmux_connect_port(devId, 62078);
    if (sess->sLock == INVALID_SOCKET) {
        Apple_StopSession(sess);
        return 0;
    }

    char startPlist[512];
    sprintf(startPlist,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>Request</key><string>StartSession</string>\n"
        "<key>HostID</key><string>%s</string>\n"
        "<key>SystemBUID</key><string>%s</string>\n"
        "</dict></plist>", hostId, buid);
    send_lockdown_plist(sess->sLock, startPlist);
    char *respStart = recv_lockdown_plist(sess->sLock);
    free(respStart);

    // TLS Handshake
    SecBuffer outSecBuf = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc outSecDesc = { SECBUFFER_VERSION, 1, &outSecBuf };
    DWORD sspiFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD outFlags = 0;

    sc = InitializeSecurityContextA(&sess->hCred, NULL, NULL, sspiFlags, 0, 0, NULL, 0, &sess->hContext, &outSecDesc, &outFlags, &ts);
    if (outSecBuf.cbBuffer > 0) {
        send(sess->sLock, (char*)outSecBuf.pvBuffer, outSecBuf.cbBuffer, 0);
        FreeContextBuffer(outSecBuf.pvBuffer);
    }

    char tlsRecvBuf[16384];
    int tlsRecvLen = 0;
    while (sc == SEC_I_CONTINUE_NEEDED || sc == SEC_E_INCOMPLETE_MESSAGE) {
        if (sc == SEC_E_INCOMPLETE_MESSAGE || tlsRecvLen == 0) {
            int r = recv(sess->sLock, tlsRecvBuf + tlsRecvLen, sizeof(tlsRecvBuf) - tlsRecvLen, 0);
            if (r <= 0) break;
            tlsRecvLen += r;
        }

        SecBuffer inSecBuf[2];
        inSecBuf[0].BufferType = SECBUFFER_TOKEN;
        inSecBuf[0].cbBuffer = tlsRecvLen;
        inSecBuf[0].pvBuffer = tlsRecvBuf;
        inSecBuf[1].BufferType = SECBUFFER_EMPTY;
        inSecBuf[1].cbBuffer = 0;
        inSecBuf[1].pvBuffer = NULL;
        SecBufferDesc inSecDesc = { SECBUFFER_VERSION, 2, inSecBuf };

        outSecBuf.cbBuffer = 0;
        outSecBuf.pvBuffer = NULL;

        sc = InitializeSecurityContextA(&sess->hCred, &sess->hContext, NULL, sspiFlags, 0, 0, &inSecDesc, 0, &sess->hContext, &outSecDesc, &outFlags, &ts);
        if (outSecBuf.cbBuffer > 0) {
            send(sess->sLock, (char*)outSecBuf.pvBuffer, outSecBuf.cbBuffer, 0);
            FreeContextBuffer(outSecBuf.pvBuffer);
        }

        if (sc == SEC_E_OK) {
            break;
        } else if (sc == SEC_I_CONTINUE_NEEDED) {
            if (inSecBuf[1].BufferType == SECBUFFER_EXTRA) {
                memmove(tlsRecvBuf, tlsRecvBuf + (tlsRecvLen - inSecBuf[1].cbBuffer), inSecBuf[1].cbBuffer);
                tlsRecvLen = inSecBuf[1].cbBuffer;
            } else {
                tlsRecvLen = 0;
            }
        }
    }

    // Clean up local cert contexts and keys (Schannel has cached its context)
    CertFreeCertificateContext(pCertContext);
    CryptDestroyKey(hRsaKey);
    CryptReleaseContext(hProv, 0);
    free(certDer); free(certPem); free(keyDer); free(keyPem); free(pairBuf);
    LocalFree(pRawRsa); LocalFree(pKeyInfo);

    if (sc != SEC_E_OK) {
        Apple_StopSession(sess);
        return 0;
    }

    // Start AFC Service
    const char *afcStartReq = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>Request</key><string>StartService</string>\n"
        "<key>Service</key><string>com.apple.afc</string>\n"
        "</dict></plist>";
    char *respAfcXml = tls_request_plist(sess, afcStartReq);
    int afcPort = 0;
    if (respAfcXml) {
        char *pPort = strstr(respAfcXml, "<key>Port</key>");
        if (pPort) {
            char *pInt = strstr(pPort, "<integer>");
            if (pInt) sscanf(pInt, "<integer>%d</integer>", &afcPort);
        }
        free(respAfcXml);
    }

    if (afcPort <= 0) {
        Apple_StopSession(sess);
        return 0;
    }

    sess->sAfc = usbmux_connect_port(devId, (unsigned short)afcPort);
    if (sess->sAfc == INVALID_SOCKET) {
        Apple_StopSession(sess);
        return 0;
    }

    return 1;
}

void Apple_StopSession(AppleSession *sess) {
    if (!sess) return;
    if (sess->sAfc != INVALID_SOCKET) {
        closesocket(sess->sAfc);
        sess->sAfc = INVALID_SOCKET;
    }
    if (sess->sNp != INVALID_SOCKET) {
        closesocket(sess->sNp);
        sess->sNp = INVALID_SOCKET;
    }
    if (sess->sLock != INVALID_SOCKET) {
        closesocket(sess->sLock);
        sess->sLock = INVALID_SOCKET;
    }
    DeleteSecurityContext(&sess->hContext);
    FreeCredentialsHandle(&sess->hCred);
}

int Apple_PostNotification(AppleSession *sess, const char *notificationName) {
    if (!sess || !notificationName) return 0;

    // Connect to notification proxy if not already connected
    if (sess->sNp == INVALID_SOCKET) {
        const char *npReq = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<plist version=\"1.0\"><dict>\n"
            "<key>Request</key><string>StartService</string>\n"
            "<key>Service</key><string>com.apple.mobile.notification_proxy</string>\n"
            "</dict></plist>";
        char *respNpXml = tls_request_plist(sess, npReq);
        int npPort = 0;
        if (respNpXml) {
            char *pPort = strstr(respNpXml, "<key>Port</key>");
            if (pPort) {
                char *pInt = strstr(pPort, "<integer>");
                if (pInt) sscanf(pInt, "<integer>%d</integer>", &npPort);
            }
            free(respNpXml);
        }
        if (npPort <= 0) return 0;
        sess->sNp = usbmux_connect_port(sess->devId, (unsigned short)npPort);
        if (sess->sNp == INVALID_SOCKET) return 0;
    }

    char notifXml[512];
    sprintf(notifXml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>Command</key><string>PostNotification</string>\n"
        "<key>Name</key><string>%s</string>\n"
        "</dict></plist>", notificationName);
    send_lockdown_plist(sess->sNp, notifXml);
    return 1;
}

int Apple_AfcOpenFile(AppleSession *sess, const char *remotePath, unsigned long long mode, unsigned long long *outHandle) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !remotePath || !outHandle) return -1;
    *outHandle = 0;

    int pathLen = (int)strlen(remotePath) + 1;
    int argsLen = 8 + pathLen;
    char *args = (char*)malloc(argsLen);
    memcpy(args, &mode, 8);
    memcpy(args + 8, remotePath, pathLen);

    unsigned long long afcHdr[5];
    afcHdr[0] = 0x4141504C36414643ULL; // "CFA6LPAA"
    afcHdr[1] = 40 + argsLen;
    afcHdr[2] = 40 + argsLen;
    afcHdr[3] = sess->afcSeq++;
    afcHdr[4] = 0x0D; // AFC_OP_FILE_OPEN

    send(sess->sAfc, (char*)afcHdr, 40, 0);
    send(sess->sAfc, args, argsLen, 0);
    free(args);

    unsigned long long resHdr[5];
    if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
    if (resHdr[4] != 0x0E) {
        if (resHdr[1] > 40) {
            char dump[64];
            int toDump = (int)(resHdr[1] - 40);
            while (toDump > 0) {
                int chk = (toDump > 64) ? 64 : toDump;
                recv_exact(sess->sAfc, dump, chk);
                toDump -= chk;
            }
        }
        return -1;
    }

    unsigned long long handle = 0;
    recv_exact(sess->sAfc, (char*)&handle, (int)(resHdr[1] - 40));
    *outHandle = handle;
    return (handle > 0) ? 0 : -1;
}

int Apple_AfcReadFile(AppleSession *sess, unsigned long long handle, void *buffer, unsigned long long len, unsigned long long *outRead) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !handle || !buffer || !outRead) return -1;
    *outRead = 0;

    unsigned long long readArgs[2] = { handle, len };
    unsigned long long afcHdr[5];
    afcHdr[0] = 0x4141504C36414643ULL;
    afcHdr[1] = 40 + 16;
    afcHdr[2] = 40 + 16;
    afcHdr[3] = sess->afcSeq++;
    afcHdr[4] = 0x0F; // AFC_OP_READ

    send(sess->sAfc, (char*)afcHdr, 40, 0);
    send(sess->sAfc, (char*)readArgs, 16, 0);

    unsigned long long resHdr[5];
    if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
    if (resHdr[4] != 0x02) {
        if (resHdr[1] > 40) {
            char dump[64];
            int toDump = (int)(resHdr[1] - 40);
            while (toDump > 0) {
                int chk = (toDump > 64) ? 64 : toDump;
                recv_exact(sess->sAfc, dump, chk);
                toDump -= chk;
            }
        }
        return -1;
    }

    int dataLen = (int)(resHdr[1] - 40);
    if (dataLen > 0) {
        recv_exact(sess->sAfc, (char*)buffer, dataLen);
    }
    *outRead = (unsigned long long)dataLen;
    return 0;
}

int Apple_AfcWriteFile(AppleSession *sess, unsigned long long handle, const void *data, unsigned long long len) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !handle || !data) return -1;

    // Stream in chunks of up to 64KB
    const char *pData = (const char*)data;
    unsigned long long rem = len;
    while (rem > 0) {
        unsigned long long chunk = (rem > 65536) ? 65536 : rem;
        unsigned long long afcHdr[5];
        afcHdr[0] = 0x4141504C36414643ULL;
        afcHdr[1] = 48 + chunk;
        afcHdr[2] = 48;
        afcHdr[3] = sess->afcSeq++;
        afcHdr[4] = 0x10; // AFC_OP_WRITE

        send(sess->sAfc, (char*)afcHdr, 40, 0);
        send(sess->sAfc, (char*)&handle, 8, 0);
        send(sess->sAfc, pData, (int)chunk, 0);

        unsigned long long resHdr[5];
        if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
        if (resHdr[1] > 40) {
            char dump[16];
            recv_exact(sess->sAfc, dump, (int)(resHdr[1] - 40));
        }
        pData += chunk;
        rem -= chunk;
    }
    return 0;
}

int Apple_AfcCloseFile(AppleSession *sess, unsigned long long handle) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !handle) return -1;

    unsigned long long afcHdr[5];
    afcHdr[0] = 0x4141504C36414643ULL;
    afcHdr[1] = 48;
    afcHdr[2] = 48;
    afcHdr[3] = sess->afcSeq++;
    afcHdr[4] = 0x14; // AFC_OP_FILE_CLOSE

    send(sess->sAfc, (char*)afcHdr, 40, 0);
    send(sess->sAfc, (char*)&handle, 8, 0);

    unsigned long long resHdr[5];
    if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
    if (resHdr[1] > 40) {
        char dump[16];
        recv_exact(sess->sAfc, dump, (int)(resHdr[1] - 40));
    }
    return 0;
}

int Apple_AfcMakeDir(AppleSession *sess, const char *remoteDir) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !remoteDir) return -1;

    int dirLen = (int)strlen(remoteDir) + 1;
    unsigned long long afcHdr[5];
    afcHdr[0] = 0x4141504C36414643ULL;
    afcHdr[1] = 40 + dirLen;
    afcHdr[2] = 40 + dirLen;
    afcHdr[3] = sess->afcSeq++;
    afcHdr[4] = 0x09; // AFC_OP_MAKE_DIR

    send(sess->sAfc, (char*)afcHdr, 40, 0);
    send(sess->sAfc, remoteDir, dirLen, 0);

    unsigned long long resHdr[5];
    if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
    if (resHdr[1] > 40) {
        char dump[16];
        recv_exact(sess->sAfc, dump, (int)(resHdr[1] - 40));
    }
    return 0;
}

int Apple_AfcRemovePath(AppleSession *sess, const char *remotePath) {
    if (!sess || sess->sAfc == INVALID_SOCKET || !remotePath) return -1;

    int pathLen = (int)strlen(remotePath) + 1;
    unsigned long long afcHdr[5];
    afcHdr[0] = 0x4141504C36414643ULL;
    afcHdr[1] = 40 + pathLen;
    afcHdr[2] = 40 + pathLen;
    afcHdr[3] = sess->afcSeq++;
    afcHdr[4] = 0x08; // AFC_OP_REMOVE_PATH

    send(sess->sAfc, (char*)afcHdr, 40, 0);
    send(sess->sAfc, remotePath, pathLen, 0);

    unsigned long long resHdr[5];
    if (recv_exact(sess->sAfc, (char*)resHdr, 40) != 40) return -1;
    if (resHdr[1] > 40) {
        char dump[16];
        recv_exact(sess->sAfc, dump, (int)(resHdr[1] - 40));
    }
    return 0;
}
