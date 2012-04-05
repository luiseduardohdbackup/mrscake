/* net.c
   model training client/server.

   Part of the data prediction package.
   
   Copyright (c) 2011 Matthias Kramm <kramm@quiss.org> 
 
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include "io.h"
#include "net.h"
#include "dataset.h"
#include "model_select.h"
#include "serialize.h"
#include "settings.h"
#include "job.h"

typedef struct _worker {
    pid_t pid;
    int start_time;
} worker_t;

typedef struct _server {
    worker_t* jobs;
    int num_workers;
} server_t;

static volatile server_t server;
static sigset_t sigchld_set;

void clean_old_workers()
{
    sigprocmask(SIG_BLOCK, &sigchld_set, 0);
    int t;
    for(t=0;t<server.num_workers;t++) {
        if(time(0) - server.jobs[t].start_time > config_remote_worker_timeout) {
            printf("killing worker %d\n", server.jobs[t].pid);
            kill(server.jobs[t].pid, 9);
        }
    }
    sigprocmask(SIG_UNBLOCK, &sigchld_set, 0);
}

static void sigchild(int signal)
{
    while(1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if(pid<0)
            break;

        int i;
        for(i=0;i<server.num_workers;i++) {
            if(pid == server.jobs[i].pid) {
                printf("worker %d: finished: %s %d\n", pid,
                        WIFEXITED(status)?"exit": (WIFSIGNALED(status)?"signal": "abnormal"),
                        WIFEXITED(status)? WEXITSTATUS(status):WTERMSIG(status)
                        );
                server.jobs[i] = server.jobs[--server.num_workers];
                break;
            }
        }
    }
}

static void process_request(int socket)
{
    reader_t*r = filereader_new(socket);

    char*name = read_string(r);
    printf("worker %d: processing model %s\n", getpid(), name);
    model_factory_t* factory = model_factory_get_by_name(name);
    if(!factory) {
        printf("worker %d: unknown factory '%s'\n", getpid(), name);
        free(name);
        return;
    }
    free(name);

    dataset_t* dataset = dataset_read(r);
    printf("worker %d: %d rows of data\n", getpid(), dataset->num_rows);

    job_t j;
    j.factory = factory;
    j.data = dataset;
    j.code = 0;
    job_process(&j);
    node_t*code = j.code;

    printf("worker %d: writing out model data\n", getpid());
    writer_t*w = filewriter_new(socket);
    node_write(code, w, SERIALIZE_DEFAULTS);
    w->finish(w);
}

int start_server(int port)
{
    struct sockaddr_in sin;
    int sock;
    int ret, i;
    int val = 1;
    memset(&sin, 0, sizeof(sin));
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;

    sock = socket(AF_INET, SOCK_STREAM, 6);
    if(sock<0) {
        perror("socket");
        exit(1);
    }
    ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof (val));
    if(ret<0) {
        perror("setsockopt");
        exit(1);
    }
    ret = bind(sock, (struct sockaddr*)&sin, sizeof(sin));
    if(ret<0) {
        perror("bind");
        exit(1);
    }
    ret = listen(sock, 5);
    if(ret<0) {
        perror("listen");
        exit(1);
    }
    ret = fcntl(sock, F_SETFL, O_NONBLOCK);
    if(ret<0) {
        perror("fcntl");
        exit(1);
    }

    server.jobs = malloc(sizeof(worker_t)*config_number_of_remote_workers);
    server.num_workers = 0;

    sigemptyset(&sigchld_set);
    sigaddset(&sigchld_set, SIGCHLD);

    signal(SIGCHLD, sigchild);

    printf("listing on port %d\n", port);
    while(1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        do {
            ret = select(sock + 1, &fds, 0, 0, 0);
        } while(ret == -1 && errno == EINTR);
        if(ret<0) {
            perror("select");
            exit(1);
        }
        if(FD_ISSET(sock, &fds)) {
            struct sockaddr_in sin;
            int len = sizeof(sin);

            int newsock = accept(sock, (struct sockaddr*)&sin, &len);
            if(newsock < 0) {
                perror("accept");
                exit(1);
            }

            // clear O_NONBLOCK
            ret = fcntl(sock, F_SETFL, 0);
            if(ret<0) {
                perror("fcntl");
                exit(1);
            }

            /* Wait for a free worker to become available. Only
               after we have a worker will we actually read the 
               job data */
            while(server.num_workers >= config_number_of_remote_workers) {
                printf("Wait for free worker (%d/%d)", server.num_workers, config_number_of_remote_workers);
                sleep(1);
                clean_old_workers();
            }

            /* block child signals while we're modifying num_workers / jobs */
            sigprocmask(SIG_BLOCK, &sigchld_set, 0);

            pid_t pid = fork();
            if(!pid) {
                sigprocmask(SIG_UNBLOCK, &sigchld_set, 0);
                process_request(newsock);
                printf("worker %d: close\n", getpid());
                close(newsock);
                _exit(0);
            }
            server.jobs[server.num_workers].pid = pid;
            server.jobs[server.num_workers].start_time = time(0);
            server.num_workers++;

            sigprocmask(SIG_UNBLOCK, &sigchld_set, 0);

            close(newsock);
        }
    }
}

int connect_to_host(const char *host, int port)
{
    int i, ret;
    char buf_ip[100];
    struct sockaddr_in sin;

    struct hostent *he = gethostbyname(host);
    if(!he) {
        fprintf(stderr, "gethostbyname returned %d\n", h_errno);
        herror(host);
        return -1;
    }

    unsigned char*ip = he->h_addr_list[0];
    //printf("Connecting to %d.%d.%d.%d:%d...\n", ip[0], ip[1], ip[2], ip[3], port);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    memcpy(&sin.sin_addr.s_addr, ip, 4);

    int sock = socket(AF_INET, SOCK_STREAM, 6);
    if(sock < 0) {
        perror("socket");
        return -1;
    }

    ret = connect(sock, (struct sockaddr*)&sin, sizeof(struct sockaddr_in));
    if(ret < 0) {
        perror("connect");
        return -1;
    }
    return sock;
}

remote_job_t* remote_job_start(const char*model_name, dataset_t*dataset)
{
    int sock;
    while(1) {
        if(!config_num_remote_servers) {
            fprintf(stderr, "No remote servers configured.\n");
            exit(1);
        }
        static int round_robin = 0;
        remote_server_t*s = &config_remote_servers[(round_robin++)%config_num_remote_servers];
        printf("Starting %s on %s\n", model_name, s->host);fflush(stdout);
        sock = connect_to_host(s->host, s->port);
        if(sock>=0) {
            break;
        }
        sleep(1);
    }

    writer_t*w = filewriter_new(sock);
    write_string(w, model_name);
    dataset_write(dataset, w);
    w->finish(w);

    remote_job_t*j = malloc(sizeof(remote_job_t));
    j->socket = sock;
    j->start_time = time(0);
    return j;
}

bool remote_job_is_ready(remote_job_t*j)
{
    struct timeval timeout;
    memset(&timeout, 0, sizeof(timeout));
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(j->socket, &readfds);
    while(1) {
        int ret = select(j->socket+1, &readfds, NULL, NULL, &timeout);
        if(ret<0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    return !!FD_ISSET(j->socket, &readfds);
}

node_t* remote_job_read_result(remote_job_t*j)
{
    reader_t*r = filereader_with_timeout_new(j->socket, config_remote_read_timeout);
    node_t*code = node_read(r);
    r->dealloc(r);
    free(j);
    return code;
}

void remote_job_cancel(remote_job_t*j)
{
    close(j->socket);
    free(j);
}

time_t remote_job_age(remote_job_t*j)
{
    return time(0) - j->start_time;
}

node_t* process_job_remotely(const char*model_name, dataset_t*dataset) //unused
{
    remote_job_t*j = remote_job_start(model_name, dataset);
    return remote_job_read_result(j);
}
