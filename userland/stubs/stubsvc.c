// onistub: registers stand-in binder services that acknowledge every call. Daemons the host can't
// run yet (netd needs kernel BPF, vold needs block devices) are replaced by services that answer
// each transaction with "no exception" and no data; Java callers read missing return values as 0,
// false, null or an empty list, which is what system_server expects from a healthy but idle daemon.
//
//   onistub NAME=INTERFACE[+EXTENSION_INTERFACE[@CODE]] ...   e.g. onistub netd=android.net.INetd vold=android.os.IVold
//   +EXT attaches a second stub as the binder's extension (how unstable AIDL companions are published);
//   @CODE:b answers that transaction code with the extension binder (IDnsResolver.getUnstableInterface);
//   @CODE:iN answers it with the int32 N (IAudioPolicyService.registerSoundTriggerCaptureStateListener -> true).
//   Several specs are separated by commas.
//
// Built with the NDK against the image's own libbinder_ndk.so (see userland/Makefile, target stubs).
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Platform-only entry points (not in the NDK headers); exported by the image's libbinder_ndk.so.
binder_exception_t AServiceManager_addService(AIBinder *binder, const char *instance);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);

#define TAG "onistub"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

typedef struct { int code; char kind; int32_t value; } Reply;   // kind 'b': the extension binder, 'i': an int32
typedef struct { const char *name, *iface; AIBinder *ext; Reply replies[16]; int n_replies; } Svc;

static void *on_create(void *args) { return args; }
static void on_destroy(void *userdata) { (void)userdata; }

// AIDL meta transactions get believable answers; everything else is a success with no payload.
static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
    (void)in;
    Svc *s = AIBinder_getUserData(binder);
    if (code == 0x00ffffff) {                     // getInterfaceVersion
        AParcel_writeInt32(out, 0);
        AParcel_writeInt32(out, 15);
        return STATUS_OK;
    }
    if (code == 0x00fffffe) {                     // getInterfaceHash
        AParcel_writeInt32(out, 0);
        AParcel_writeString(out, "notfrozen", 9);
        return STATUS_OK;
    }
    if (code == 0x5f504e47) {                     // PING_TRANSACTION
        return STATUS_OK;
    }
    LOGI("%s: transaction %u acknowledged", s->name, (unsigned)code);
    AParcel_writeInt32(out, 0);                   // exception header: none
    for (int i = 0; i < s->n_replies; i++) {
        if ((int)code != s->replies[i].code) continue;
        if (s->replies[i].kind == 'b' && s->ext) AParcel_writeStrongBinder(out, s->ext);
        else if (s->replies[i].kind == 'i') AParcel_writeInt32(out, s->replies[i].value);
    }
    return STATUS_OK;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: onistub NAME=INTERFACE ...\n"); return 2; }
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    for (int i = 1; i < argc; i++) {
        Svc *s = calloc(1, sizeof *s);
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "onistub: %s: expected NAME=INTERFACE\n", argv[i]); return 2; }
        *eq = 0;
        s->name = argv[i];
        s->iface = eq + 1;
        char *plus = strchr(eq + 1, '+');
        if (plus) *plus = 0;
        char *at = plus ? strchr(plus + 1, '@') : NULL;
        if (at) {
            *at = 0;
            for (char *t = strtok(at + 1, ","); t && s->n_replies < 16; t = strtok(NULL, ",")) {
                char *colon = strchr(t, ':');
                Reply *r = &s->replies[s->n_replies++];
                r->code = atoi(t);
                r->kind = colon && colon[1] == 'i' ? 'i' : 'b';
                r->value = colon && colon[1] == 'i' ? atoi(colon + 2) : 0;
            }
        }
        AIBinder_Class *cls = AIBinder_Class_define(s->iface, on_create, on_destroy, on_transact);
        AIBinder_Class_disableInterfaceTokenHeader(cls);   // callers may use variant descriptors (…unstable.descriptor.…)
        AIBinder *b = AIBinder_new(cls, s);
        if (plus) {
            Svc *x = calloc(1, sizeof *x);
            x->name = s->name;
            x->iface = plus + 1;
            AIBinder_Class *xcls = AIBinder_Class_define(x->iface, on_create, on_destroy, on_transact);
            AIBinder_Class_disableInterfaceTokenHeader(xcls);
            AIBinder *xb = AIBinder_new(xcls, x);
            AIBinder_setExtension(b, xb);
            s->ext = xb;
            LOGI("%s: extension %s attached", s->name, x->iface);
        }
        binder_exception_t e = AServiceManager_addService(b, s->name);
        LOGI("registered %s (%s): %d", s->name, s->iface, (int)e);
        if (e != EX_NONE) fprintf(stderr, "onistub: addService(%s) failed: %d\n", s->name, (int)e);
    }
    ABinderProcess_joinThreadPool();
    return 0;
}
