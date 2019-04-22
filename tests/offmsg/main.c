#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/time.h>
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

#define MSG_INACTIVE_TIMEOUT   30
#define CONFIG_NAME   "offmsg_tests.conf"

static const char *config_files[] = {
    "./"CONFIG_NAME,
    NULL
};

enum  {
    RunMode_unkown = 0,
    RunMode_sender = 1,
    RunMode_receiver = 2
};

enum {
    Task_unkown = 0,
    Task_init = 4,
    Task_addfriend = 5,
    Task_offmsg = 6
};

typedef struct TestCtx {
    char remote_addr[ELA_MAX_ADDRESS_LEN + 1];
    char remote_userid[ELA_MAX_ID_LEN + 1];
    bool connected;
    bool did_kill;

    FILE *offmsg_fp;

    int mode;
    int tasktype;
    int error;
    struct timeval last_timestamp;
} TestCtx;

const char *mode_str[] = {
    "unknown",
    "sender",
    "receiver"
};

static void output_null(const char *format, va_list args) {}
static void output_addr_userid(const char *addr, const char *userid)
{
    printf("%s:%s\n", addr, userid);
}

static void output_error()
{
    output_addr_userid("error", "error");
}

static void try_send_offmsg(ElaCarrier *w, TestCtx *ctx)
{
    char buf[128] = {0};
    int error =  0;
    int rc = 0;

    rc = fread(buf, sizeof(buf), 1, ctx->offmsg_fp);
    if (rc != 1) {
        if  (!feof(ctx->offmsg_fp)) {
            vlogE("Error: read offmsg error: (%s)", ferror(ctx->offmsg_fp));
            error = 1;
        }
        goto error_exit;
    }

    rc = ela_send_friend_message(w, ctx->remote_userid, buf, rc);
    if (rc < 0) {
        vlogE("Error: Send offline message error: 0x%x", ela_get_error());
        goto error_exit;
    }

    return;

error_exit:
    ctx->error = error;
    ctx->did_kill = true;
    ela_kill(w);
}

static void try_store_offmsg(ElaCarrier *w, TestCtx *ctx,
                             const void *msg, size_t length)
{
    size_t rc = 0;

    rc = fwrite(msg, length, 1, ctx->offmsg_fp);
    if (rc != 1) {
        vlogE("Error: write offmsg error (%s)", ferror(ctx->offmsg_fp));
        ctx->error = 1;
        ctx->did_kill = true;
        ela_kill(w);
    } else {
        gettimeofday(&ctx->last_timestamp, NULL);
    }
}

static void idle_callback(ElaCarrier *w, void *context)
{
    TestCtx *ctx = (TestCtx *)context;

    if (!ctx->connected || !ela_is_ready(w))
        return;

    switch(ctx->tasktype) {
    case Task_addfriend: {
        int rc = 0;

        if (!ela_is_friend(w, ctx->remote_userid)) {
            rc = ela_add_friend(w, ctx->remote_userid, "offmsg_tests");
            if (rc < 0)
                vlogE("Error: Add friend error: 0x%x", ela_get_error());
        }

        ctx->error = (rc == 0 ? 0 : 1);
        ctx->did_kill = true;
        ela_kill(w);
        break;
    }

    case Task_offmsg:
        if (ctx->mode == RunMode_sender)
            try_send_offmsg(w, ctx);
        else {
            struct timeval now = {0};
            struct timeval timeout_interval = {0};
            struct timeval expiration = {0};

            gettimeofday(&now, NULL);
            timeout_interval.tv_sec = MSG_INACTIVE_TIMEOUT;
            timeout_interval.tv_usec = 0;
            timeradd(&ctx->last_timestamp, &timeout_interval, &expiration);
            if (timercmp(&now, &expiration, >)) {
                ctx->error = 1;
                ctx->did_kill = true;
                ela_kill(w);
            }
        }
        break;

    default:
        break;
    }
}

static void connection_callback(ElaCarrier *w, ElaConnectionStatus status,
                                void *context)
{
    TestCtx *ctx = (TestCtx *)context;
    ctx->connected = (status == ElaConnectionStatus_Connected);
}

static void message_callback(ElaCarrier *w, const char *from,
                             const void *msg, size_t len, void *context)
{
    TestCtx *ctx = (TestCtx *)context;

    vlogI("Message from friend[%s]: %.*s", from, (int)len, msg);

    if (ctx->tasktype == Task_offmsg && ctx->mode == RunMode_receiver)
        try_store_offmsg(w, ctx, msg, len);
}

static void ready_callback(ElaCarrier *w, void *context)
{
    TestCtx *ctx = (TestCtx *)context;
    gettimeofday(&ctx->last_timestamp, NULL);
}

static void usage(void)
{
    printf("usage: offmsg_tests [option] --sender | --receiver.\n"
           "\n"
           "    -c, --config[=<config-path>\n"
           "                     the pathname of test config file\n"
           "    --init           try to create carrier instance, and print its\n"
           "                     address and userid\n"
           "    --add-friend     try to add new friend.\n"
           "    --message        try to send offline message (for sender) or\n"
           "                     receive offline message (for receiver)\n"
           "\n"
           "    --sender         the role to send offline messages\n"
           "    --receiver       the role to receive offline messages\n"
           "\n"
           "    -a, --remote-address=<remote-address>\n"
           "                     the address of remote tests node\n"
           "    -u, --remote-userid=<remote-userid>\n"
           "                     the userid of remote tests node\n"
           "\n"
           "    --refmsg=<offmsg-text>\n"
           "                     the text file that sender read from and send\n"
           "                     it as offline message to remote tests node\n"
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

const char *get_config_path(const char *cfg_file, const char *cfg_files[])
{
    const char **file = cfg_file ? &cfg_file : cfg_files;

    for (; *file; ) {
        int fd = open(*file, O_RDONLY);
        if (fd < 0) {
            if (*file == cfg_file)
                file = cfg_files;
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
    TestConfig *config = NULL;
    const char *config_file = NULL;
    char logfile[PATH_MAX] = {0};
    char datadir[PATH_MAX] = {0};
    char refmsg_path[PATH_MAX] = {0};
    TestCtx testctx = {0};
    TestCtx *ctx = &testctx;
    int tasktype = 0;
    int mode = 0;
    int level;
    int rc = -1;
    int i = 0;
    int debug = 0;
    int opt = 0;
    int idx = 0;
    struct option options[] = {
        { "sender",         no_argument,        NULL,  1 },
        { "receiver",       no_argument,        NULL,  2 },
        { "debug",          no_argument,        NULL,  3 },
        { "init",           no_argument,        NULL,  4 },
        { "add-friend",     no_argument,        NULL,  5 },
        { "message",        no_argument,        NULL,  6 },
        { "remote-address", required_argument,  NULL, 'a' },
        { "remote-userid",  required_argument,  NULL, 'u' },
        { "refmsg",         required_argument,  NULL, 'm' },
        { "config",         required_argument,  NULL, 'c' },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

    memset(ctx, 0, sizeof(TestCtx));

#ifdef HAVE_SYS_RESOURCE_H
    sys_coredump_set(true);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while ((opt = getopt_long(argc, argv, "a:u:c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 1:
        case 2:
            if (mode > 0) {
                output_error();
                return -1;
            }

            mode = opt;
            break;

        case 3:
            debug = 1;
            break;

        case 4:
        case 5:
        case 6:
            if (tasktype > 0) {
                output_error();
                return -1;
            }

            tasktype = opt;
            break;

        case 'a':
            if (optarg)
                strncpy(ctx->remote_addr, optarg, ELA_MAX_ADDRESS_LEN);
            break;

        case 'u':
            if (optarg)
                strncpy(ctx->remote_userid, optarg, ELA_MAX_ID_LEN);
            break;

        case 'm':
            if (optarg)
                strncpy(refmsg_path, optarg, PATH_MAX - 1);
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

    if (!mode || !tasktype) {
        output_error();
        return -1;
    }

    if (tasktype == Task_addfriend && (!*ctx->remote_addr || !*ctx->remote_userid)) {
        output_error();
        return -1;
    }

    if (tasktype == Task_offmsg && !*ctx->remote_userid) {
        output_error();
        return -1;
    }

    ctx->did_kill = false;
    ctx->mode = mode;
    ctx->tasktype = tasktype;

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
    if (!config_file || mode == RunMode_unkown) {
        vlogE("Error: Missing config file");
        output_error();
        goto error_exit;
     }

    config = load_config(config_file);
    if (!config) {
        vlogE("Error: Loading config file.");
        output_error();
        goto error_exit;
    }

    memset(&opts, 0, sizeof(opts));
    sprintf(logfile, "%s/%s.log", config->data_location, mode_str[mode]);

    if (mode == RunMode_sender)
        level = config->sender_log_level;
    else
        level = config->receiver_log_level;

    vlog_init(level, logfile, output_null);

    opts.udp_enabled = config->udp_enabled;
    sprintf(datadir, "%s/%s", config->data_location, mode_str[mode]);
    opts.persistent_location = datadir;
    opts.dht_bootstraps_size = config->bootstraps_size;
    opts.dht_bootstraps = (DhtBootstrapNode *)calloc(1,
                    sizeof(DhtBootstrapNode) * opts.dht_bootstraps_size);
    if (!opts.dht_bootstraps) {
        output_error();
        goto error_exit;
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
    opts.hive_bootstraps = (HiveBootstrapNode *)calloc(1,
                     sizeof(HiveBootstrapNode) * opts.hive_bootstraps_size);
    if (!opts.hive_bootstraps) {
        output_error();
        free(opts.dht_bootstraps);
        goto error_exit;
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
    callbacks.friend_message = message_callback;
    callbacks.ready = ready_callback;

    w = ela_new(&opts, &callbacks, ctx);
    free(opts.dht_bootstraps);
    free(opts.hive_bootstraps);

    if (!w) {
        vlogE("Create carrier instance error: 0x%x", ela_get_error());
        output_error();
        goto error_exit;
    }

    if (tasktype == Task_init) {
        char addr[ELA_MAX_ADDRESS_LEN + 1];
        char userid[ELA_MAX_ID_LEN + 1];

        ela_get_address(w, addr, sizeof(addr));
        ela_get_userid(w, userid, sizeof(userid));
        output_addr_userid(addr, userid);
        goto error_exit;
    }

    if (tasktype == Task_offmsg) {
        FILE *fp;
        char *open_mode[] = {
            NULL,
            "rb",
            "wb"
        };

        fp = fopen(refmsg_path, open_mode[mode]);

        if (!fp)
            goto error_exit;

        ctx->offmsg_fp = fp;
    }

    rc = ela_run(w, 10);
    if (rc != 0) {
        vlogE("Run carrier instance error: 0x%x", ela_get_error());
        ctx->error = rc;
    }

error_exit:
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    if (ctx->offmsg_fp)
        fclose(ctx->offmsg_fp);

    if (w && !ctx->did_kill)
        ela_kill(w);

    if (config)
        deref(config);

    return ctx->error;
}
