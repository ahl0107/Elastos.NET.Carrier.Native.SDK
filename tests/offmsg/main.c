#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#ifdef __linux__
#define __USE_GNU
#include <pthread.h>
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#else
#include <pthread.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#endif

#include <ela_carrier.h>
#include <crystal.h>

#include "config.h"

#define CONFIG_NAME   "offmsg_tests.conf"

static const char *config_files[] = {
    "./"CONFIG_NAME,
    "../etc/carrier/"CONFIG_NAME,
#if !defined(_WIN32) && !defined(_WIN64)
    "/usr/local/etc/carrier/"CONFIG_NAME,
    "/etc/carrier/"CONFIG_NAME,
#endif
    NULL
};

#define KILL_RECEIVER       "kill_receiver"
#define RESTART_RECEIVER    "restart_receiver"

#define STREQ(a, b) (strcmp((a), (b)) == 0)

typedef enum RUNNING_MODE {
    UNKNOWN_MODE,
    SENDER_MODE,
    RECEIVER_MODE,
    LAUNCHER_MODE
} RUNNING_MODE;

typedef struct SenderArgs {
    bool has_sent_msg;
    bool is_friend_online;
} SenderArgs;

typedef struct ReceiverArgs {
    char recv_msg[32];
    bool to_recv_msg;
    bool has_get_msg;
    bool has_set_address;
} ReceiverArgs;

typedef struct NodeArgs {
    union address {
        char self[ELA_MAX_ADDRESS_LEN + 1];
        char peer[ELA_MAX_ADDRESS_LEN + 1];
    } address;
    bool has_connected;
    bool to_kill_carrier;
    char peer_id[ELA_MAX_ID_LEN+1];
    void *mode_args;
    RUNNING_MODE mode;
} NodeArgs;

static const char *g_msg = "messagetest";

pthread_mutex_t g_screen_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
pthread_mutex_t g_carrier_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static void *node_main(void *args);

// static void output(const char *format, ...)
// {
//     va_list args;

//     va_start(args, format);

//     pthread_mutex_lock(&g_screen_lock);
//     vfprintf(stderr, format, args);
//     pthread_mutex_unlock(&g_screen_lock);

//     va_end(args);
// }

const char *presence_name[] = {
    "none",    // None;
    "away",    // Away;
    "busy",    // Busy;
};

static void friend_add(ElaCarrier *w, void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;
    int rc;

    rc = ela_add_friend(w, node_args->address.peer, "Hello");
    if (rc == 0) {
        vlogI("Request to add a new friend successfully.\n");
    }
    else
        vlogE("Request to add a new friend unsuccessfully(0x%x).\n",
                ela_get_error());
}

static void friend_accept(ElaCarrier *w, const char *user_id)
{
    int rc;

    rc = ela_accept_friend(w, user_id);
    if (rc == 0)
        vlogI("Accept friend request successfully.\n");
    else
        vlogE("Accept friend request unsuccessfully(0x%x).\n", ela_get_error());
}

static void friend_remove(ElaCarrier *w, void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;
    int rc;

    rc = ela_remove_friend(w, node_args->peer_id);
    if (rc == 0)
        vlogI("Remove friend %s successfully.\n", node_args->peer_id);
    else
        vlogE("Remove friend %s unsuccessfully(0x%x).\n", node_args->peer_id, ela_get_error());
}

static int first_friends_item = 1;

static const char *connection_name[] = {
    "online",
    "offline"
};

/* This callback share by list_friends and global
 * friend list callback */
static bool friends_list_callback(ElaCarrier *w, const ElaFriendInfo *friend_info,
                                 void *context)
{
    static int count;

    if (first_friends_item) {
        count = 0;
        vlogI("Friend list from carrier network:\n");
        vlogI("  %-46s %8s %s\n", "ID", "Connection", "Label");
        vlogI("  %-46s %8s %s\n", "----------------", "----------", "-----");
    }

    if (friend_info) {
        vlogI("  %-46s %8s %s\n", friend_info->user_info.userid,
               connection_name[friend_info->status], friend_info->label);
        first_friends_item = 0;
        count++;
    } else {
        /* The list ended */
        vlogI("  ----------------\n");
        vlogI("Total %d friends.\n", count);

        first_friends_item = 1;
    }

    return true;
}

/* This callback share by list_friends and global
 * friend list callback */
static void display_friend_info(const ElaFriendInfo *fi)
{
    vlogI("           ID: %s\n", fi->user_info.userid);
    vlogI("         Name: %s\n", fi->user_info.name);
    vlogI("  Description: %s\n", fi->user_info.description);
    vlogI("       Gender: %s\n", fi->user_info.gender);
    vlogI("        Phone: %s\n", fi->user_info.phone);
    vlogI("        Email: %s\n", fi->user_info.email);
    vlogI("       Region: %s\n", fi->user_info.region);
    vlogI("        Label: %s\n", fi->label);
    vlogI("     Presence: %s\n", presence_name[fi->presence]);
    vlogI("   Connection: %s\n", connection_name[fi->status]);
}

static void friend_added_callback(ElaCarrier *w, const ElaFriendInfo *info,
                                  void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;

    strcpy(node_args->peer_id, info->user_info.userid);
    vlogI("New friend added:\n");
    display_friend_info(info);
}

static void friend_removed_callback(ElaCarrier *w, const char *friendid,
                                    void *context)
{
    vlogI("Friend %s removed!\n", friendid);
}

static void idle_callback(ElaCarrier *w, void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;
    static int first_time = 1;
    bool to_kill_carrier = false;

    if (node_args->has_connected && (node_args->mode == SENDER_MODE)
        && (ela_is_ready(w)) && (first_time == 1)) {

        if (strlen(node_args->address.peer) > 0) {
            char *p = NULL;

            p = ela_get_id_by_address(node_args->address.peer, node_args->peer_id, ELA_MAX_ID_LEN + 1);
            if (p != NULL) {
                friend_remove(w, context);
                friend_add(w, context);
                first_time = 0;
            } else {
                vlogE("Got user ID unsuccessfully.\n");
            }
        }
    }

    pthread_mutex_lock(&g_carrier_lock);
    to_kill_carrier = node_args->to_kill_carrier;
    pthread_mutex_unlock(&g_carrier_lock);

    if (to_kill_carrier)
        ela_kill(w);
}

static void connection_callback(ElaCarrier *w, ElaConnectionStatus status,
                                void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;

    switch (status) {
    case ElaConnectionStatus_Connected:
        node_args->has_connected = true;
        vlogI("Connected to carrier network.\n");
        break;

    case ElaConnectionStatus_Disconnected:
        node_args->has_connected = false;
        vlogI("Disconnected from carrier network.\n");
        break;

    default:
        vlogE("Error!!! Got unknown connection status %d.\n", status);
    }
}

static void friend_info_callback(ElaCarrier *w, const char *friendid,
                                 const ElaFriendInfo *info, void *context)
{
    vlogI("Friend information changed:\n");
    display_friend_info(info);
}

static void friend_connection_callback(ElaCarrier *w, const char *friendid,
                                       ElaConnectionStatus status, void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;

    switch (status) {
    case ElaConnectionStatus_Connected:
        vlogI("Friend[%s] connection changed to be online\n", friendid);

        if ((STREQ(node_args->peer_id, friendid)) && (node_args->mode == SENDER_MODE)) {
            SenderArgs *sender_args = (SenderArgs*)node_args->mode_args;

            pthread_mutex_lock(&g_carrier_lock);
            sender_args->is_friend_online = true;
            pthread_mutex_unlock(&g_carrier_lock);
        }
        break;

    case ElaConnectionStatus_Disconnected:
        vlogI("Friend[%s] connection changed to be offline.\n", friendid);

        if ((STREQ(node_args->peer_id, friendid)) && (node_args->mode == SENDER_MODE)) {
            int ret = 0;
            SenderArgs *sender_args = (SenderArgs*)node_args->mode_args;

            ret = ela_send_friend_message(w, friendid, g_msg, strlen(g_msg));
            pthread_mutex_lock(&g_carrier_lock);
            sender_args->has_sent_msg = true;
            pthread_mutex_unlock(&g_carrier_lock);
            if (ret < 0)
                vlogE("Send a message unsuccessfully.\n");
            else
                vlogI("Send a message successfully.\n");
        }
        break;

    default:
        vlogE("Error!!! Got unknown connection status %d.\n", status);
    }
}

static void friend_request_callback(ElaCarrier *w, const char *userid,
                                    const ElaUserInfo *info, const char *hello,
                                    void *context)
{
    vlogI("Friend request from user[%s] with HELLO: %s.\n",
           *info->name ? info->name : userid, hello);

    friend_accept(w, userid);
}

static void message_callback(ElaCarrier *w, const char *from,
                             const void *msg, size_t len, void *context)
{
    NodeArgs *node_args = (NodeArgs*)context;

    vlogI("Message from friend[%s]: %.*s\n", from, (int)len, msg);

    if (node_args->mode == RECEIVER_MODE) {
        ReceiverArgs *receiver_args = (ReceiverArgs*)node_args->mode_args;

        if (receiver_args->to_recv_msg) {
            strncpy(receiver_args->recv_msg, (char *)msg, len);
            pthread_mutex_lock(&g_carrier_lock);
            receiver_args->has_get_msg = true;
            pthread_mutex_unlock(&g_carrier_lock);
            node_args->to_kill_carrier = true;
        }
    }
}

static void ready_callback(ElaCarrier *w, void *context)
{
    vlogI("ready_callback invoked\n");
}

static void usage(void)
{
    printf("Carrier offline message tests.\n");
    printf("\n");
    printf("Run offline message tests.\n");
    printf("Usage: offmsg_tests -c CONFIG\n");
    printf("\n");
    printf("Launch offline message sender\n");
    printf("Usage: offmsg_tests -c CONFIG --sender --from=YOUR-PATH [--addr=REMOTE-ADDR --uid=REMOTE-USERID]\n");
    printf("\n");
    printf("Launch offline message receiver\n");
    printf("Usage: offmsg_tests -c CONFIG --receiver --to=YOUR-PATH\n");
    printf("\n");
}

#ifdef HAVE_SYS_RESOURCE_H
int sys_coredump_set(bool enable)
{
    const struct rlimit rlim = {
        enable ? RLIM_INFINITY : 0,
        enable ? RLIM_INFINITY : 0
    };

    return setrlimit(RLIMIT_CORE, &rlim);
}
#endif

void wait_for_debugger_attach(void)
{
    printf("\nWait for debugger attaching, process id is: %d.\n", getpid());
    printf("After debugger attached, press any key to continue......");
#if defined(_WIN32) || defined(_WIN64)
    DebugBreak();
#else
    getchar();
#endif
}

static void signal_handler(int signum)
{
    printf("Got signal: %d, force exit.\n", signum);
    exit(-1);
}

static void log_init(TestConfig *test_config, int mode)
{
    char filename[PATH_MAX];
    char *log_file;
    int level;

    if (mode == SENDER_MODE) {
        sprintf(filename, "%s/sender.log", test_config->data_location);
        log_file = filename;
        level = test_config->sender_log_level;
    } else if (mode == RECEIVER_MODE) {
        sprintf(filename, "%s/receiver.log", test_config->data_location);
        log_file = filename;
        level = test_config->receiver_log_level;
    } else {
        log_file = NULL;
        level = VLOG_INFO;
    }

    vlog_init(level, log_file, NULL);
}

void reset_options()
{
    optind = 1;
}

const char *get_config_path(const char *config_file, const char *config_files[])
{
    const char **file = config_file ? &config_file : config_files;

    for (; *file; ) {
        int fd = open(*file, O_RDONLY);
        if (fd < 0) {
            if (*file == config_file)
                file = config_files;
            else
                file++;

            continue;
        }

        close(fd);

        return *file;
    }

    return NULL;
}

int sender_main(int argc, char *argv[])
{
    int ret = 0;
    int opt = 0;
    int idx = 0;
    char buf[32] = {0};
    pthread_t sender_thread;
    NodeArgs node_args = {0};
    SenderArgs sender_args = {0};
    struct option options[] = {
        { "sender",         no_argument,        NULL, 1 },
        { "debug",          no_argument,        NULL, 2 },
        { "addr",           required_argument,  NULL, 'a' },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    reset_options();
    while ((opt = getopt_long(argc, argv, "a:c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 'a':
            strncpy(node_args.address.peer, optarg, ELA_MAX_ADDRESS_LEN);
            break;

        case 'h':
        case '?':
            usage();
            return -1;
        }
    }

    vlogI("I am the sender.\n");
    vlogI("Got the receiver's address:%s\n", node_args.address.peer);

    node_args.mode = SENDER_MODE;
    sender_args.has_sent_msg = false;
    sender_args.is_friend_online = false;
    node_args.mode_args = &sender_args;
    ret = pthread_create(&sender_thread, NULL, node_main, &node_args);
    if (ret != 0) {
        vlogE("Create an sender thread unsuccessfully.\n");
        return -1;
    }

    vlogI("Wait for the receiver to be online.\n");
wait_for_friend_online:
    pthread_mutex_lock(&g_carrier_lock);
    if (!sender_args.is_friend_online) {
        pthread_mutex_unlock(&g_carrier_lock);
        sleep(1);
        goto wait_for_friend_online;
    }
    pthread_mutex_unlock(&g_carrier_lock);

    // Tell the parent process to let the receiver to exit.
    vlogI("Tell the parent to kill the receiver.\n");
    fprintf(stdout, "%s\n", KILL_RECEIVER);
    fflush(stdout);

    vlogI("Wait for the message to be sent.\n");
wait_for_sending_msg:
    pthread_mutex_lock(&g_carrier_lock);
    if (!sender_args.has_sent_msg) {
        pthread_mutex_unlock(&g_carrier_lock);
        sleep(1);
        goto wait_for_sending_msg;
    }
    pthread_mutex_unlock(&g_carrier_lock);

    // Tell the parent to restart the receiver.
    fprintf(stdout, "%s\n", RESTART_RECEIVER);
    fflush(stdout);

    pthread_mutex_lock(&g_carrier_lock);
    node_args.to_kill_carrier = true;
    pthread_mutex_unlock(&g_carrier_lock);

    pthread_join(sender_thread, NULL);

    vlogI("Sender(%d) exited.\n", getpid());

    return 0;
}

int receiver_main(int argc, char *argv[])
{
    int ret = 0;
    int opt = 0;
    int idx = 0;
    bool to_recv_msg = false;
    char buf[32] = {0};
    pthread_t receiver_thread;
    NodeArgs node_args = {0};
    ReceiverArgs receiver_args = {0};
    struct option options[] = {
        { "receiver",        no_argument,        NULL, 1 },
        { "recv",           no_argument,        NULL, 2 },
        { "debug",          no_argument,        NULL, 3 },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    reset_options();
    while ((opt = getopt_long(argc, argv, "c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 2:
            to_recv_msg = true;
            break;

        case 'h':
        case '?':
            usage();
            return -1;
        }
    }

    vlogI("I am the receiver.\n");

    node_args.mode = RECEIVER_MODE;
    receiver_args.has_set_address = false;
    receiver_args.to_recv_msg = to_recv_msg;
    node_args.mode_args = &receiver_args;
    ret = pthread_create(&receiver_thread, NULL, node_main, &node_args);
    if (ret != 0) {
        vlogE("Create a receiver thread unsuccessfully.\n");
        return -1;
    }

    if (to_recv_msg) {
    wait_for_getting_msg:
        pthread_mutex_lock(&g_carrier_lock);
        if (!receiver_args.has_get_msg) {
            pthread_mutex_unlock(&g_carrier_lock);
            sleep(1);
            goto wait_for_getting_msg;
        }
        pthread_mutex_unlock(&g_carrier_lock);

        fprintf(stdout, "%s\n", receiver_args.recv_msg);
        fflush(stdout);

        goto receiver_exit;
    }

    vlogI("Wait for the address to be set.\n");
wait_for_address:
    pthread_mutex_lock(&g_carrier_lock);
    if (!receiver_args.has_set_address) {
        pthread_mutex_unlock(&g_carrier_lock);
        sleep(1);
        goto wait_for_address;
    }
    pthread_mutex_unlock(&g_carrier_lock);

    vlogI("My address:%s\n", node_args.address.self);
    fprintf(stdout, "%s\n", node_args.address.self);
    fflush(stdout);

    vlogI("The receiver is waiting to be killed.\n");
wait_kill_from_parent:
    fscanf(stdin, "%s", buf);
    vlogI("The receiver got data from the parent:%s.\n", buf);
    if (STREQ(buf, KILL_RECEIVER)) {
        pthread_mutex_lock(&g_carrier_lock);
        node_args.to_kill_carrier = true;
        pthread_mutex_unlock(&g_carrier_lock);
    } else {
        goto wait_kill_from_parent;
    }

receiver_exit:
    pthread_join(receiver_thread, NULL);

    vlogI("Receiver(%d) exited.\n", getpid());

    return 0;
}

int launcher_main(int argc, char *argv[])
{
    int rc = 0;
    int i = 0;
    char buf[32] = {0};
    char cmdbase[1024] = {0};
    char cmdline[1024] = {0};
    char receiver_address[ELA_MAX_ADDRESS_LEN + 1] = {0};
    subprocess_t sender_node = NULL;
    subprocess_t receiver_node = NULL;

    vlogI("I am launcher.\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    for (; i < argc; i++) {
        if (STREQ(argv[i], "--debug"))
            continue;

        strcat(cmdbase, argv[i]);
        strcat(cmdbase, " ");
    }

    sprintf(cmdline, "%s --receiver", cmdbase);
    receiver_node = spopen(cmdline, "r+");
    if (receiver_node == NULL) {
        rc = -1;
        goto cleanup;
    }

    vlogI("Begin to wait for the receiver's address.\n");
    fscanf(spstdout(receiver_node), "%s", receiver_address);
    vlogI("Got the receiver's address:%s.\n", receiver_address);

    sprintf(cmdline, "%s --sender", cmdbase);
    sprintf(cmdline, "%s -a %s", cmdline, receiver_address);
    sender_node = spopen(cmdline, "r+");
    if (sender_node == NULL) {
        spkill(receiver_node);
        spclose(receiver_node);
        rc = -1;
        goto cleanup;
    }

wait_kill_from_sender:
    fscanf(spstdout(sender_node), "%s", buf);
    vlogI("Got data from the sender:%s.\n", buf);
    if (STREQ(buf, KILL_RECEIVER)) {
        fprintf(spstdin(receiver_node), "%s\n", KILL_RECEIVER);
        fflush(spstdin(receiver_node));
    } else {
        goto wait_kill_from_sender;
    }

    spclose(receiver_node);
    receiver_node = NULL;

wait_restart_from_sender:
    memset(buf, 0, sizeof(buf));
    fscanf(spstdout(sender_node), "%s", buf);
    vlogI("Got data from the sender:%s.\n", buf);
    if (STREQ(buf, RESTART_RECEIVER)) {
        memset(cmdline, 0, sizeof(cmdline));
        sprintf(cmdline, "%s --receiver --recv", cmdbase);
        receiver_node = spopen(cmdline, "r+");
        if (receiver_node == NULL) {
            rc = -1;
            goto cleanup;
        }

        memset(buf, 0, sizeof(buf));
        fscanf(spstdout(receiver_node), "%s", buf);
        vlogI("Got data(message) from the receiver:%s.\n", buf);
        spclose(receiver_node);
        receiver_node = NULL;
        if (STREQ(buf, g_msg))
            vlogI("This test case ran successfully.\n");
        else
            vlogE("This test case ran unsuccessfully.\n");
    } else{
        goto wait_restart_from_sender;
    }

    rc = 0;

cleanup:
    return rc;
}

int main(int argc, char *argv[])
{
    int rc = 0;
    int debug = 0;
    char buffer[PATH_MAX] = {0};
    const char *config_file = NULL;
    RUNNING_MODE mode = UNKNOWN_MODE;
    int opt = 0;
    int idx = 0;
    struct option options[] = {
        { "sender",         no_argument,        NULL, 1 },
        { "receiver",        no_argument,        NULL, 2 },
        { "recv",           no_argument,        NULL, 3 },
        { "debug",          no_argument,        NULL, 4 },
        { "addr",           required_argument,  NULL, 'a' },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while ((opt = getopt_long(argc, argv, "a:c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 1:
        case 2:
            if (mode != UNKNOWN_MODE) {
                printf("Error: Conflict arguments.\n");
                usage();
                return -1;
            }

            mode = opt;
            break;

        case 4:
            debug = 1;
            break;

        case 'c':
            config_file = optarg;
            break;

        case 'h':
        case '?':
            usage();
            return -1;
        }
    }

    if (debug)
        wait_for_debugger_attach();

#if defined(_WIN32) || defined(_WIN64)
    WORD wVersionRequested;
    WSADATA wsaData;

    wVersionRequested = MAKEWORD(2, 0);

    if (WSAStartup(wVersionRequested, &wsaData) != 0)
        return -1;
#endif

    config_file = get_config_path(config_file, config_files);

    if (!config_file) {
        printf("Error: Missing config file.\n");
        usage();
        return -1;
     }

    if (mode == UNKNOWN_MODE)
        mode = LAUNCHER_MODE;

    // The primary job: load configuration file
    global_config = load_config(config_file);

    log_init(global_config, mode);

    switch (mode) {
    case SENDER_MODE:
        rc = sender_main(argc, argv);
        break;

    case RECEIVER_MODE:
        rc = receiver_main(argc, argv);
        break;

    case LAUNCHER_MODE:
        realpath(argv[0], buffer);
        argv[0] = buffer;
        rc = launcher_main(argc, argv);
        break;

    default:
        break;
    }

#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    deref(global_config);

    return rc;
}

static void *node_main(void *args)
{
    ElaCarrier *w = NULL;
    ElaOptions opts = {0};
    NodeArgs *node_args = (NodeArgs*)args;
    char buf[ELA_MAX_ADDRESS_LEN+1] = {0};
    char datadir[PATH_MAX] = {0};
    ElaCallbacks callbacks = {0};
    int log_level = 0;
    int rc = 0;
    int i = 0;

    memset(&opts, 0, sizeof(opts));

    opts.udp_enabled = global_config->udp_enabled;
    if (node_args->mode == SENDER_MODE) {
        log_level = global_config->sender_log_level;
        sprintf(datadir, "%s/sender", global_config->data_location);
    } else {
        log_level = global_config->receiver_log_level;
        sprintf(datadir, "%s/receiver", global_config->data_location);
    }
    opts.persistent_location = datadir;
    opts.dht_bootstraps_size = global_config->bootstraps_size;
    opts.dht_bootstraps = (DhtBootstrapNode *)calloc(1, sizeof(DhtBootstrapNode) * opts.dht_bootstraps_size);
    if (!opts.dht_bootstraps) {
        fprintf(stderr, "out of memory.");
        return (void*)-1;
    }

    for (i = 0 ; i < opts.dht_bootstraps_size; i++) {
        DhtBootstrapNode *b = &opts.dht_bootstraps[i];
        DhtBootstrapNode *node = global_config->bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
        b->public_key = node->public_key;
    }

    opts.hive_bootstraps_size = global_config->hive_bootstraps_size;
    opts.hive_bootstraps = (HiveBootstrapNode *)calloc(1, sizeof(HiveBootstrapNode) * opts.hive_bootstraps_size);
    if (!opts.hive_bootstraps) {
        fprintf(stderr, "out of memory.");
        free(opts.dht_bootstraps);
        return (void*)-1;
    }

    for (i = 0 ; i < global_config->hive_bootstraps_size; i++) {
        HiveBootstrapNode *b = &opts.hive_bootstraps[i];
        HiveBootstrapNode *node = global_config->hive_bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.idle = idle_callback;
    callbacks.connection_status = connection_callback;
    callbacks.friend_list = friends_list_callback;
    callbacks.friend_connection = friend_connection_callback;
    callbacks.friend_info = friend_info_callback;
    callbacks.friend_presence = NULL;
    callbacks.friend_request = friend_request_callback;
    callbacks.friend_added = friend_added_callback;
    callbacks.friend_removed = friend_removed_callback;
    callbacks.friend_message = message_callback;
    callbacks.friend_invite = NULL;
    callbacks.ready = ready_callback;

    w = ela_new(&opts, &callbacks, args);
    free(opts.dht_bootstraps);
    free(opts.hive_bootstraps);

    if (!w) {
        vlogE("Error create carrier instance: 0x%x\n", ela_get_error());
        vlogE("Press any key to quit...");
        goto quit;
    }

    vlogI("Carrier node identities:\n");
    vlogI("   Node ID: %s\n", ela_get_nodeid(w, buf, sizeof(buf)));
    vlogI("   User ID: %s\n", ela_get_userid(w, buf, sizeof(buf)));
    vlogI("   Address: %s\n\n", ela_get_address(w, buf, sizeof(buf)));
    vlogI("\n");

    vlogI("pid:%d, mode:%d\n", getpid(), node_args->mode);
    if (node_args->mode == RECEIVER_MODE) {
        ReceiverArgs *receiver_args = (ReceiverArgs*)node_args->mode_args;

        if (!receiver_args->to_recv_msg) {
            strcpy(node_args->address.self, buf);
            pthread_mutex_lock(&g_carrier_lock);
            receiver_args->has_set_address = true;
            pthread_mutex_unlock(&g_carrier_lock);
        }
    }

    rc = ela_run(w, 10);
    if (rc != 0) {
        vlogE("Error start carrier loop: 0x%x\n", ela_get_error());
        vlogE("Press any key to quit...");
        ela_kill(w);
        goto quit;
    }

quit:
    return (void*)0;
}