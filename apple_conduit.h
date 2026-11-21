#ifndef APPLE_CONDUIT_H
#define APPLE_CONDUIT_H

#include <winsock2.h>
#include <windows.h>

#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>

#define AFC_MODE_RDONLY   1
#define AFC_MODE_RW       2
#define AFC_MODE_WRONLY   3
#define AFC_MODE_WR       4

typedef struct {
    int devId;
    char udid[64];
    SOCKET sLock;
    CtxtHandle hContext;
    CredHandle hCred;
    SOCKET sAfc;
    SOCKET sNp;
    unsigned long long afcSeq;
} AppleSession;

// Device discovery & connection
int Apple_FindDevice(int *outDevId, char *outUdid, int maxUdid);
int Apple_StartSession(AppleSession *sess, int devId, const char *udid);
void Apple_StopSession(AppleSession *sess);

// Notification Proxy
int Apple_PostNotification(AppleSession *sess, const char *notificationName);

// AFC File operations
int Apple_AfcOpenFile(AppleSession *sess, const char *remotePath, unsigned long long mode, unsigned long long *outHandle);
int Apple_AfcReadFile(AppleSession *sess, unsigned long long handle, void *buffer, unsigned long long len, unsigned long long *outRead);
int Apple_AfcWriteFile(AppleSession *sess, unsigned long long handle, const void *data, unsigned long long len);
int Apple_AfcCloseFile(AppleSession *sess, unsigned long long handle);
int Apple_AfcMakeDir(AppleSession *sess, const char *remoteDir);
int Apple_AfcRemovePath(AppleSession *sess, const char *remotePath);

#endif // APPLE_CONDUIT_H
