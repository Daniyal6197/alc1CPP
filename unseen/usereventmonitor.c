/*
 * userevent monitor client for the Homework Database
 */

#include "config.h"
#include "util.h"
#include "rtab.h"
#include "srpc.h"
#include "timestamp.h"
#include "usereventmonitor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <time.h>
#include <sys/time.h>

#include <signal.h>

#define USAGE "./usereventmonitor [-h host] [-p port]"
#define TIME_DELTA 5 /* in seconds */

static struct timespec time_delay = {TIME_DELTA, 0};
static int must_exit = 0;
static int exit_status = 0;
static int sig_received;

static tstamp_t processresults(char *buf, unsigned int len);

static void signal_handler(int signum) {
    must_exit = 1;
    sig_received = signum;
}

int main(int argc, char *argv[]) {
    RpcConnection rpc;
    Q_Decl(query,SOCK_RECV_BUF_LEN);
    char resp[SOCK_RECV_BUF_LEN];
    int qlen;
    unsigned len;
    char *host;
    unsigned short port;
    int i, j;
    struct timeval expected, current;
    tstamp_t last = 0LL;

    host = HWDB_SERVER_ADDR;
    port = HWDB_SERVER_PORT;
    for (i = 1; i < argc; ) {
        if ((j = i + 1) == argc) {
            fprintf(stderr, "usage: %s\n", USAGE);
            exit(1);
        }
        if (strcmp(argv[i], "-h") == 0)
            host = argv[j];
        else 
	    if (strcmp(argv[i], "-p") == 0)
            port = atoi(argv[j]);
	else {
            fprintf(stderr, "Unknown flag: %s %s\n", argv[i], argv[j]);
        }
        i = j + 1;
    }
    /* first attempt to connect to the database server */
    if (!rpc_init(0)) {
        fprintf(stderr, "Failure to initialize rpc system\n");
        exit(-1);
    }
    if (!(rpc = rpc_connect(host, port, "HWDB", 1l))) {
        fprintf(stderr, "Failure to connect to HWDB at %s:%05u\n", host, port);
        exit(-1);
    }
    
    /* now establish signal handlers to gracefully exit from loop */
    if (signal(SIGTERM, signal_handler) == SIG_IGN)
        signal(SIGTERM, SIG_IGN);
    if (signal(SIGINT, signal_handler) == SIG_IGN)
        signal(SIGINT, SIG_IGN);
    if (signal(SIGHUP, signal_handler) == SIG_IGN)
        signal(SIGHUP, SIG_IGN);
    gettimeofday(&expected, NULL);
    expected.tv_usec = 0;
    while (! must_exit) {
        tstamp_t last_seen;
        expected.tv_sec += TIME_DELTA;
        if (last) {
            char *s = timestamp_to_string(last);
            sprintf(query,
"SQL:select * from UserEvents [ since %s ]\n", s);
            free(s);
        } else
            sprintf(query, 
"SQL:select * from UserEvents [ range %d seconds ]\n", TIME_DELTA);
        qlen = strlen(query) + 1;
        gettimeofday(&current, NULL);
        if (current.tv_usec > 0) {
            time_delay.tv_nsec = 1000 * (1000000 - current.tv_usec);
            time_delay.tv_sec = expected.tv_sec - current.tv_sec - 1;
        } else {
            time_delay.tv_nsec = 0;
            time_delay.tv_sec = expected.tv_sec - current.tv_sec;
        }
        nanosleep(&time_delay, NULL);
        if (! rpc_call(rpc, Q_Arg(query), qlen, resp, sizeof(resp), &len)) {
            fprintf(stderr, "rpc_call() failed\n");
            break;
        }
        resp[len] = '\0';
        if ((last_seen = processresults(resp, len)))
            last = last_seen;
    }
    rpc_disconnect(rpc);
    exit(exit_status);
}

UserEventResults *mon_convert(Rtab *results) {
    UserEventResults *ans;
    unsigned int i;

    if (! results || results->mtype != 0)
        return NULL;
    if (!(ans = (UserEventResults *)malloc(sizeof(UserEventResults))))
        return NULL;
    ans->nevents = results->nrows;
    ans->data = (UserEvent **)calloc(ans->nevents, sizeof(UserEvent *));
    if (! ans->data) {
        free(ans);
        return NULL;
    }
    for (i = 0; i < ans->nevents; i++) {
        char **columns;
        UserEvent *p = (UserEvent *)malloc(sizeof(UserEvent));
        if (!p) {
            mon_free(ans);
            return NULL;
        }
        ans->data[i] = p;
        columns = rtab_getrow(results, i);
        p->tstamp = string_to_timestamp(columns[0]);
	memset(p->application, 0, sizeof(p->application));
        strncpy(p->application, columns[1], sizeof(p->application)-1);
	memset(p->logtype, 0, sizeof(p->logtype));
        strncpy(p->logtype, columns[2], sizeof(p->logtype)-1);
	memset(p->logdata, 0, sizeof(p->logdata));
        strncpy(p->logdata, columns[3], sizeof(p->logdata)-1);
    }
    return ans;
}

void mon_free(UserEventResults *p) {
    unsigned int i;

    if (p) {
        for (i = 0; i < p->nevents && p->data[i]; i++)
            free(p->data[i]);
        free(p);
    }
}

static tstamp_t processresults(char *buf, unsigned int len) {
    Rtab *results;
    char stsmsg[RTAB_MSG_MAX_LENGTH];
    UserEventResults *p;
    unsigned long i;
    tstamp_t last = 0LL;

    results = rtab_unpack(buf, len);
    if (results && ! rtab_status(buf, stsmsg)) {
        p = mon_convert(results);
        /* do something with the data pointed to by p */
        printf("Retrieved %ld user event records from database\n", p->nevents);
        for (i = 0; i < p->nevents; i++) {
            UserEvent *f = p->data[i];
            char *s = timestamp_to_string(f->tstamp);
	    printf("%s %s:%s:%s\n", 
		s, f->application, f->logtype, f->logdata);
	    free(s);
        }
        if (i > 0) {
            i--;
            last = p->data[i]->tstamp;
        }
        mon_free(p);
    }
    rtab_free(results);
    return (last);
}
