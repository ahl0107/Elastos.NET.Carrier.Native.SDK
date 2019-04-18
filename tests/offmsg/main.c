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
#include <time.h>

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

#define MSG_INACTIVE_TIMEOUT   30
#define CONFIG_NAME   "offmsg_tests.conf"

static const char *config_files[] = {
    "./"CONFIG_NAME,
    NULL
};

typedef enum RUNNING_MODE {
    UNKNOWN_MODE,
    SENDER_MODE,
    RECEIVER_MODE
} RUNNING_MODE;

typedef enum ACTION {
    UNKNOWN_ACTION,
    INIT_ACTION,
    FRIEND_ACTION,
    SEND_MSG_ACTION,
    RECV_MSG_ACTION
} ACTION;

typedef struct Bundle {
    char peer_address[ELA_MAX_ADDRESS_LEN + 1];
    char peer_id[ELA_MAX_ID_LEN + 1];
    int fd;
    time_t last_active_time;
    bool connected;
    bool friend_connected;
    ACTION action;
    RUNNING_MODE mode;
} Bundle;

const char *mode_str[] = {
    "unknown",
    "sender",
    "receiver"
};

static void friend_add(ElaCarrier *w, void *context)
{
    Bundle *b = (Bundle*)context;
    int rc;

    rc = ela_add_friend(w, b->peer_address, "Hello");
    if (rc == 0)
        vlogI("Request to add a new friend successfully.");
    else
        vlogE("Request to add a new friend unsuccessfully(0x%x).",
              ela_get_error());
}

static void output_null(const char *format, va_list args) {}

static void output_addr_userid(const char *addr, const char *userid)
{
    printf("%s:%s\n", addr, userid);
}

static void output_error()
{
    output_addr_userid("error", "error");
}

static void send_msg_from_file(ElaCarrier *w, void *context)
{
    Bundle *b = (Bundle*)context;
    char buf[1024] = {0};
    int ret = 0;

    ret = read(b->fd, buf, sizeof(buf));
    if (ret > 0) {
        ret = ela_send_friend_message(w, b->peer_id, buf, ret);
        if (ret == 0)
            vlogI("Send a message successfully.");
        else
            vlogE("Send a message unsucessfully.");
    } else {
        close(b->fd);
        ela_kill(w);
    }
}

static void save_msg_to_file(ElaCarrier *w, const void *msg, size_t len, void *context)
{
    Bundle *b = (Bundle*)context;
    int ret = 0;

    ret = write(b->fd, msg, len);
    if (ret < 0) {
        close(b->fd);
        ela_kill(w);
    }
}

static void idle_callback(ElaCarrier *w, void *context)
{
    Bundle *b = (Bundle*)context;

    if ((b->action == FRIEND_ACTION) && b->connected && ela_is_ready(w)) {
        if (!ela_is_friend(w, b->peer_id))
            friend_add(w, context);

        ela_kill(w);
    } else {
        if ((b->action == SEND_MSG_ACTION) && b->connected && !b->friend_connected && ela_is_ready(w))
            send_msg_from_file(w, context);
        else if (b->action == RECV_MSG_ACTION) {
            if (time(NULL) - b->last_active_time >= MSG_INACTIVE_TIMEOUT) {
                close(b->fd);
                ela_kill(w);
            }
        } else {
            // Do nothing.
        }
    }
}

static void connection_callback(ElaCarrier *w, ElaConnectionStatus status,
                                void *context)
{
    Bundle *b = (Bundle*)context;

    b->connected = (status == ElaConnectionStatus_Connected) ? true : false;

    switch (status) {
    case ElaConnectionStatus_Connected:
        vlogI("Connected to carrier network.");
        break;

    case ElaConnectionStatus_Disconnected:
        vlogI("Disconnected from carrier network.");
        break;

    default:
        vlogE("Error!!! Got unknown connection status %d.", status);
    }
}

static void friend_connection_callback(ElaCarrier *w, const char *friendid,
                                       ElaConnectionStatus status, void *context)
{
    Bundle *b = (Bundle*)context;

    b->friend_connected = (status == ElaConnectionStatus_Connected) ? true : false;

    switch (status) {
    case ElaConnectionStatus_Connected:
        vlogI("Friend[%s] connection changed to be online.", friendid);
        break;

    case ElaConnectionStatus_Disconnected:
        vlogI("Friend[%s] connection changed to be offline.", friendid);
        break;

    default:
        vlogE("Error!!! Got unknown connection status %d.", status);
    }
}

static void message_callback(ElaCarrier *w, const char *from,
                             const void *msg, size_t len, void *context)
{
    Bundle *b = (Bundle*)context;

    vlogI("Message from friend[%s]: %.*s", from, (int)len, msg);

    if ((b->action == RECV_MSG_ACTION) && strcmp(from, b->peer_id) == 0 && len > 0) {
        save_msg_to_file(w, msg, len, context);
        b->last_active_time = time(NULL);
    }
}

static void ready_callback(ElaCarrier *w, void *context)
{
    Bundle *b = (Bundle*)context;

    if (b->action == RECV_MSG_ACTION)
        b->last_active_time = time(NULL);
}

static void usage(void)
{
    printf("usage: offmsg_tests [option] --sender | --receiver.\n"
           "\n"
           "    -c, --config[=<config-path>\n"
           "                     test pathname of config file\n"
           "    --init           create carrier instance, and print its \n"
           "                     address and userid\n"
           "\n"
           "    --sender         the role to send offline messages\n"
           "    --receiver       the role to receive offline messages\n"
           "\n"
           "    -a, --remote-address=<remote-address>\n"
           "                     the address of remote tests node\n"
           "    -u, --remote-userid=<remote-userid>\n"
           "                     the userid of remote tests node\n"
           "\n"
           "    --refmsg-from=<offmsg-text>\n"
           "                     the text file that sender read from and send\n"
           "                     it as offline message to remote tests node"
           "    --refmsg-to=<offmsg-text>\n"
           "                     the text file that receiver store offline\n"
           "                     offline message into after receiving them\n"
           "                     them from tests node\n"
           "\n");
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

int main(int argc, char *argv[])
{
    ElaOptions opts = {0};
    ElaCallbacks callbacks = {0};
    ElaCarrier *w = NULL;
    Bundle bundle = {0};
    RUNNING_MODE mode = UNKNOWN_MODE;
    ACTION action = UNKNOWN_ACTION;
    TestConfig *config = NULL;
    const char *config_file = NULL;
    const char *msg_file = NULL;
    char address[ELA_MAX_ADDRESS_LEN + 1] = {0};
    char userid[ELA_MAX_ID_LEN + 1] = {0};
    char datadir[PATH_MAX] = {0};
    char logfile[PATH_MAX] = {0};
    char msg_input[PATH_MAX] = {0};
    char msg_output[PATH_MAX] = {0};
    int level;
    int rc = 0;
    int i = 0;
    int debug = 0;
    int opt = 0;
    int idx = 0;
    struct option options[] = {
        { "sender",         no_argument,        NULL, 1 },
        { "receiver",       no_argument,        NULL, 2 },
        { "debug",          no_argument,        NULL, 3 },
        { "init",           no_argument,        NULL, 4 },
        { "remote-address", required_argument,  NULL, 'a' },
        { "remote-userid",  required_argument,  NULL, 'u' },
        { "refmsg-from",    required_argument,  NULL, 'f' },
        { "refmsg-to",      required_argument,  NULL, 't' },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while ((opt = getopt_long(argc, argv, "a:u:c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 1:
        case 2:
            if (mode != UNKNOWN_MODE) {
                output_error();
                return -1;
            }

            mode = opt;
            break;

        case 3:
            debug = 1;
            break;

        case 4:
            action = INIT_ACTION;
            break;

        case 'a':
            if (optarg)
                strncpy(bundle.peer_address, optarg, ELA_MAX_ADDRESS_LEN);

            break;

        case 'u':
            if (optarg)
                strncpy(bundle.peer_id, optarg, ELA_MAX_ID_LEN);

            break;

        case 'f':
            if (strlen(msg_input) > 0) {
                usage();
                return -1;
            }

            if (optarg)
                strncpy(msg_input, optarg, sizeof(msg_input) - 1);

            break;

        case 't':
            if (strlen(msg_output) > 0) {
                usage();
                return -1;
            }

            if (optarg)
                strncpy(msg_output, optarg, sizeof(msg_output) - 1);

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

    if ((action == UNKNOWN_ACTION && (mode == SENDER_MODE || mode == RECEIVER_MODE))
        && strlen(bundle.peer_address) > 0 && strlen(bundle.peer_id) > 0) {
        if (mode == SENDER_MODE && strlen(msg_input) > 0)
            action = SEND_MSG_ACTION;
        else if (mode == RECEIVER_MODE && strlen(msg_output) > 0)
            action = RECV_MSG_ACTION;
        else if ((mode == SENDER_MODE && strlen(msg_output) > 0)
                || (mode == RECEIVER_MODE && strlen(msg_input) > 0));
        else
            action = FRIEND_ACTION;
    }

    if (mode == UNKNOWN_MODE || action == UNKNOWN_ACTION) {
        output_error();
        return -1;
    }

#if defined(_WIN32) || defined(_WIN64)
    WORD wVersionRequested;
    WSADATA wsaData;

    wVersionRequested = MAKEWORD(2, 0);

    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
        output_error();
        return -1;
    }
#endif

    config_file = get_config_path(config_file, config_files);
    if (!config_file || mode == UNKNOWN_MODE) {
        vlogE("Error: Missing config file");
        output_error();
        return -1;
     }

    config = load_config(config_file);

    memset(&opts, 0, sizeof(opts));
    sprintf(logfile, "%s/%s.log", config->data_location, mode_str[mode]);

    if (mode == SENDER_MODE)
        level = config->sender_log_level;
    else
        level = config->receiver_log_level;

    vlog_init(level, logfile, output_null);

    opts.udp_enabled = config->udp_enabled;
    sprintf(datadir, "%s/%s", config->data_location, mode_str[mode]);
    opts.persistent_location = datadir;
    opts.dht_bootstraps_size = config->bootstraps_size;
    opts.dht_bootstraps = (DhtBootstrapNode *)calloc(1, sizeof(DhtBootstrapNode) * opts.dht_bootstraps_size);
    if (!opts.dht_bootstraps) {
        output_error();
        return -1;
    }

    for (i = 0 ; i < opts.dht_bootstraps_size; i++) {
        DhtBootstrapNode *b = &opts.dht_bootstraps[i];
        DhtBootstrapNode *node = config->bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
        b->public_key = node->public_key;
    }

    opts.hive_bootstraps_size = config->hive_bootstraps_size;
    opts.hive_bootstraps = (HiveBootstrapNode *)calloc(1, sizeof(HiveBootstrapNode) * opts.hive_bootstraps_size);
    if (!opts.hive_bootstraps) {
        output_error();
        free(opts.dht_bootstraps);
        return -1;
    }

    for (i = 0 ; i < config->hive_bootstraps_size; i++) {
        HiveBootstrapNode *b = &opts.hive_bootstraps[i];
        HiveBootstrapNode *node = config->hive_bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.idle = idle_callback;
    callbacks.connection_status = connection_callback;
    callbacks.friend_connection = friend_connection_callback;
    callbacks.friend_message = message_callback;
    callbacks.ready = ready_callback;

    bundle.connected = false;
    bundle.friend_connected = false;
    bundle.action = action;
    bundle.mode = mode;
    w = ela_new(&opts, &callbacks, &bundle);
    free(opts.dht_bootstraps);
    free(opts.hive_bootstraps);

    if (!w) {
        vlogE("Create carrier instance error: 0x%x", ela_get_error());
        output_error();
        rc = -1;
        goto quit;
    }

    if (action == INIT_ACTION) {
        ela_get_address(w, address, sizeof(address));
        ela_get_userid(w, userid, sizeof(userid));
        output_addr_userid(address, userid);
        ela_kill(w);
        rc = 0;
        goto quit;
    }

    if (action == SEND_MSG_ACTION)
        msg_file = msg_input;
    if (action == RECV_MSG_ACTION)
        msg_file = msg_output;

    if (msg_file) {
        bundle.fd = open(msg_file, O_RDONLY);
        if (bundle.fd < 0) {
            ela_kill(w);
            rc = -1;
            goto quit;
        }
    }

    rc = ela_run(w, 10);
    if (rc != 0) {
        vlogE("Run carrier instance error: 0x%x", ela_get_error());
        if (bundle.fd > 0)
            close(bundle.fd);

        ela_kill(w);
        goto quit;
    }

quit:
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    deref(config);

    return rc;
}
