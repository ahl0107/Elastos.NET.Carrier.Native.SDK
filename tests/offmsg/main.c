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

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#endif

#include <ela_carrier.h>
#include <crystal.h>

#include "config.h"

#define CONFIG_NAME   "offmsg_tests.conf"

static const char *config_files[] = {
    "./"CONFIG_NAME,
    NULL
};

#define STREQ(a, b) (strcmp((a), (b)) == 0)

typedef enum RUNNING_MODE {
    UNKNOWN_MODE,
    SENDER_MODE,
    RECEIVER_MODE
} RUNNING_MODE;

static void output(const char *format, va_list args)
{
    // Do nothing.
}

const char *presence_name[] = {
    "none",    // None;
    "away",    // Away;
    "busy",    // Busy;
};

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
}

static void connection_callback(ElaCarrier *w, ElaConnectionStatus status,
                                void *context)
{
    switch (status) {
    case ElaConnectionStatus_Connected:
        vlogI("Connected to carrier network.\n");
        break;

    case ElaConnectionStatus_Disconnected:
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
    switch (status) {
    case ElaConnectionStatus_Connected:
        vlogI("Friend[%s] connection changed to be online\n", friendid);
        break;

    case ElaConnectionStatus_Disconnected:
        vlogI("Friend[%s] connection changed to be offline.\n", friendid);
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
}

static void message_callback(ElaCarrier *w, const char *from,
                             const void *msg, size_t len, void *context)
{
    vlogI("Message from friend[%s]: %.*s\n", from, (int)len, msg);
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

    vlog_init(level, log_file, output);
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

void print_user_info(char *address, char *id)
{
    if (!address || !id)
        printf("error:error\n");
    else
        printf("%s:%s\n", address, id);
}

int main(int argc, char *argv[])
{
    ElaOptions opts = {0};
    ElaCallbacks callbacks = {0};
    ElaCarrier *w = NULL;
    RUNNING_MODE mode = UNKNOWN_MODE;
    const char *config_file = NULL;
    char address[ELA_MAX_ADDRESS_LEN + 1] = {0};
    char id[ELA_MAX_ID_LEN + 1] = {0};
    char datadir[PATH_MAX] = {0};
    int log_level = 0;
    int rc = 0;
    int i = 0;
    int debug = 0;
    int generate_info = 0;
    int opt = 0;
    int idx = 0;
    struct option options[] = {
        { "sender",         no_argument,        NULL, 1 },
        { "receiver",       no_argument,        NULL, 2 },
        { "debug",          no_argument,        NULL, 3 },
        { "init",           no_argument,        NULL, 4 },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while ((opt = getopt_long(argc, argv, "c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 1:
        case 2:
            if (mode != UNKNOWN_MODE) {
                vlogE("Error: Conflict arguments.\n");
                print_user_info(NULL, NULL);
                usage();
                return -1;
            }

            mode = opt;
            break;

        case 3:
            debug = 1;
            break;

        case 4:
            generate_info = 1;
            break;

        case 'c':
            config_file = optarg;
            break;

        case 'h':
        case '?':
            print_user_info(NULL, NULL);
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
        vlogE("Error: Missing config file.\n");
        print_user_info(NULL, NULL);
        usage();
        return -1;
     }

    if (mode == UNKNOWN_MODE) {
        print_user_info(NULL, NULL);
        usage();
        return -1;
    }

    // The primary job: load configuration file
    global_config = load_config(config_file);
    log_init(global_config, mode);

    memset(&opts, 0, sizeof(opts));
    opts.udp_enabled = global_config->udp_enabled;
    if (mode == SENDER_MODE) {
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
        vlogE("out of memory.");
        print_user_info(NULL, NULL);
        return -1;
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
        vlogE("out of memory.");
        print_user_info(NULL, NULL);
        free(opts.dht_bootstraps);
        return -1;
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

    w = ela_new(&opts, &callbacks, NULL);
    free(opts.dht_bootstraps);
    free(opts.hive_bootstraps);

    if (!w) {
        vlogE("Error create carrier instance: 0x%x\n", ela_get_error());
        vlogE("Press any key to quit...");
        print_user_info(NULL, NULL);
        rc = -1;
        goto quit;
    }

    if (generate_info) {
        ela_get_address(w, address, sizeof(address));
        ela_get_userid(w, id, sizeof(id));
        print_user_info(address, id);
        ela_kill(w);
        rc = 0;
        goto quit;
    }

    rc = ela_run(w, 10);
    if (rc != 0) {
        vlogE("Error start carrier loop: 0x%x\n", ela_get_error());
        vlogE("Press any key to quit...");
        ela_kill(w);
        rc = -1;
        goto quit;
    }

quit:
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    deref(global_config);

    return rc;
}