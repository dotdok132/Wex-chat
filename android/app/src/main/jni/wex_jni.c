#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "wex_ws_server.h"
#include "wex_nat.h"
#include "wex_persist.h"

#define TAG "WexNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static pthread_t s_daemon_thread;
static volatile bool s_thread_running = false;
static volatile bool s_stop_requested = false;

typedef struct {
    char data_dir[512];
    char nickname[64];
    char bootstrap[256];
    int ws_port;
    int udp_port;
} daemon_args_t;

static daemon_args_t s_args;

static void *daemon_worker_thread(void *arg) {
    (void)arg;
    LOGI("[*] Starting Wex daemon thread. DataDir=%s, WS=%d, UDP=%d, Nick=%s",
         s_args.data_dir, s_args.ws_port, s_args.udp_port, s_args.nickname);

    wex_mkdir_p(s_args.data_dir);

    char key_path[576], journal_path[576];
    snprintf(key_path, sizeof(key_path), "%s/identity.key", s_args.data_dir);
    snprintf(journal_path, sizeof(journal_path), "%s/messages.log", s_args.data_dir);

    wex_identity_t ident;
    if (wex_identity_load_or_create(&ident, key_path, s_args.nickname) != 0) {
        LOGE("[-] Failed to load or create identity at %s", key_path);
        s_thread_running = false;
        return NULL;
    }
    LOGI("[+] Identity loaded. Public key initialized, nick=%s", ident.nickname);

    wex_journal_t *journal = NULL;
    if (wex_journal_open(&journal, journal_path) == 0) {
        LOGI("[+] Journal opened at %s", journal_path);
    }

    wex_server_t *server = (wex_server_t *)calloc(1, sizeof(wex_server_t));
    if (!server) {
        LOGE("[-] Failed to allocate wex_server_t on heap");
        if (journal) wex_journal_close(journal);
        s_thread_running = false;
        return NULL;
    }

    if (wex_server_init(server, s_args.ws_port, s_args.udp_port, ident.nickname) != 0) {
        LOGE("[-] Failed to initialize Wex server");
        free(server);
        if (journal) wex_journal_close(journal);
        s_thread_running = false;
        return NULL;
    }

    server->identity = ident;
    server->p2p.identity = &server->identity;
    memcpy(server->p2p.dht.local_id, server->identity.sign_pk, 32);
    wex_p2p_set_data_dir(&server->p2p, s_args.data_dir);

    if (journal) {
        server->journal = journal;
        wex_p2p_set_journal(&server->p2p, journal);
    }

    if (wex_server_start(server) != 0) {
        LOGE("[-] Failed to start Wex server on WS port %d, UDP %d", s_args.ws_port, s_args.udp_port);
        free(server);
        if (journal) wex_journal_close(journal);
        s_thread_running = false;
        return NULL;
    }

    /* Auto STUN WAN probe in background */
    struct sockaddr_in ext_addr;
    if (wex_nat_auto_discover(server->p2p.udp_fd, &ext_addr) == 0) {
        char ext_str[64];
        wex_addr_to_string(&ext_addr, ext_str, sizeof(ext_str));
        server->p2p.external_addr = ext_addr;
        server->p2p.has_external_addr = true;
        LOGI("[+] STUN public endpoint: %s", ext_str);
    }

    /* Optional bootstrap peer or invite URI */
    if (s_args.bootstrap[0] != '\0') {
        LOGI("[*] Connecting to bootstrap target %s", s_args.bootstrap);
        wex_p2p_join_invite(&server->p2p, s_args.bootstrap);
    }

    /* Send immediate LAN discovery beacon */
    wex_p2p_send_lan_beacon(&server->p2p);

    LOGI("[+] Wex daemon running event loop");
    s_thread_running = true;

    while (!s_stop_requested) {
        wex_server_poll(server, 100);
    }

    LOGI("[*] Stopping Wex server...");
    wex_server_stop(server);
    free(server);
    if (journal) {
        wex_journal_close(journal);
    }

    s_thread_running = false;
    LOGI("[+] Wex daemon stopped cleanly");
    return NULL;
}

JNIEXPORT jint JNICALL
Java_net_wexchat_MainActivity_startNativeDaemon(JNIEnv *env, jobject thiz,
                                               jstring jDataDir,
                                               jint wsPort,
                                               jint udpPort,
                                               jstring jNickname,
                                               jstring jBootstrap) {
    (void)thiz;
    if (s_thread_running) {
        LOGI("Daemon already running");
        return 0;
    }

    const char *data_dir = (*env)->GetStringUTFChars(env, jDataDir, NULL);
    const char *nickname = (*env)->GetStringUTFChars(env, jNickname, NULL);
    const char *bootstrap = jBootstrap ? (*env)->GetStringUTFChars(env, jBootstrap, NULL) : NULL;

    memset(&s_args, 0, sizeof(s_args));
    strncpy(s_args.data_dir, data_dir ? data_dir : "/data/local/tmp", sizeof(s_args.data_dir) - 1);
    strncpy(s_args.nickname, (nickname && nickname[0]) ? nickname : "WexUser", sizeof(s_args.nickname) - 1);
    if (bootstrap) {
        strncpy(s_args.bootstrap, bootstrap, sizeof(s_args.bootstrap) - 1);
    }
    s_args.ws_port = (wsPort > 0) ? wsPort : 9000;
    s_args.udp_port = (udpPort > 0) ? udpPort : 9001;

    if (data_dir) (*env)->ReleaseStringUTFChars(env, jDataDir, data_dir);
    if (nickname) (*env)->ReleaseStringUTFChars(env, jNickname, nickname);
    if (bootstrap) (*env)->ReleaseStringUTFChars(env, jBootstrap, bootstrap);

    s_stop_requested = false;
    if (pthread_create(&s_daemon_thread, NULL, daemon_worker_thread, NULL) != 0) {
        LOGE("Failed to create daemon pthread");
        return -1;
    }

    return 0;
}

JNIEXPORT jint JNICALL
Java_net_wexchat_WexService_startNativeDaemon(JNIEnv *env, jobject thiz,
                                              jstring jDataDir,
                                              jint wsPort,
                                              jint udpPort,
                                              jstring jNickname,
                                              jstring jBootstrap) {
    return Java_net_wexchat_MainActivity_startNativeDaemon(env, thiz, jDataDir, wsPort, udpPort, jNickname, jBootstrap);
}

JNIEXPORT void JNICALL
Java_net_wexchat_MainActivity_stopNativeDaemon(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (s_thread_running) {
        s_stop_requested = true;
        pthread_join(s_daemon_thread, NULL);
    }
}

JNIEXPORT void JNICALL
Java_net_wexchat_WexService_stopNativeDaemon(JNIEnv *env, jobject thiz) {
    Java_net_wexchat_MainActivity_stopNativeDaemon(env, thiz);
}

JNIEXPORT jboolean JNICALL
Java_net_wexchat_MainActivity_isNativeDaemonRunning(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    return s_thread_running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_net_wexchat_WexService_isNativeDaemonRunning(JNIEnv *env, jobject thiz) {
    return Java_net_wexchat_MainActivity_isNativeDaemonRunning(env, thiz);
}
