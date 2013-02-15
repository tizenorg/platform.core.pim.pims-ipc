/*
 * PIMS IPC
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>

#define NAME "dom_sock_test"

#define REPEAT_COUNT    100
//#define BUFFER_SIZE 1024*1024
#define BUFFER_SIZE 1024

#define PLUS_TIME(a, b, c) do {\
    a.tv_sec = b.tv_sec + c.tv_sec;\
    a.tv_usec = b.tv_usec + c.tv_usec;\
    if (a.tv_usec >= 1000000) {\
        a.tv_sec++;\
        a.tv_usec -= 1000000;\
    }\
} while (0)

#define MINUS_TIME(a, b, c) do {\
    a.tv_sec = b.tv_sec - c.tv_sec;\
    a.tv_usec = b.tv_usec - c.tv_usec;\
    if (a.tv_usec < 0) {\
        a.tv_sec--;\
        a.tv_usec += 1000000;\
    }\
} while (0)


static char buffer[BUFFER_SIZE+1] = {0,};

void server_main()
{
    int sock, msgsock, rval;
    struct sockaddr_un server;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("opening stream socket");
        exit(1);
    }

    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, NAME);
    if (bind(sock, (struct sockaddr *) &server, sizeof(struct sockaddr_un))) {
        perror("binding stream socket");
        exit(1);
    }
    printf("Socket has name %s\n", server.sun_path);
    listen(sock, 5);
    for (;;) {
        msgsock = accept(sock, 0, 0);
        if (msgsock == -1)
            perror("accept");
        else do {
            if ((rval = read(msgsock, buffer, BUFFER_SIZE)) < 0)
                perror("reading stream message");
            else if (rval == 0)
                printf("Ending connection\n");
            else
            {
                if ((rval = write(msgsock, buffer, BUFFER_SIZE)) < 0)
                    perror("writing stream message");

            }
        } while (rval > 0);
        close(msgsock);
    }
    close(sock);
    unlink(NAME);
}

void client_main()
{
    int i;
    int sock;
    struct sockaddr_un server;
    struct timeval start_tv = {0, 0};
    struct timeval end_tv = {0, 0};
    struct timeval diff_tv = {0, 0};
    struct timeval prev_sum_tv = {0, 0};
    struct timeval sum_tv = {0, 0};
    int count = 0;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("opening stream socket");
        exit(1);
    }
    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, NAME);

    for (i = 0; i < BUFFER_SIZE; i++)
        buffer[i] = 'a';
    buffer[i] = 0;

    if (connect(sock, (struct sockaddr *) &server, sizeof(struct sockaddr_un)) < 0) {
        close(sock);
        perror("connecting stream socket");
        exit(1);
    }
    for (i = 0; i < REPEAT_COUNT; i++)
    {

        gettimeofday(&start_tv, NULL);
        if (write(sock, buffer, BUFFER_SIZE) < 0)
            perror("writing on stream socket");
        if (read(sock, buffer, BUFFER_SIZE) < 0)
            perror("read on stream socket");
        gettimeofday(&end_tv, NULL);
        MINUS_TIME(diff_tv, end_tv, start_tv);
        PLUS_TIME(sum_tv, diff_tv, prev_sum_tv);
        prev_sum_tv = sum_tv;
        count++;
        printf("start[%lu:%lu] end[%lu:%lu] diff[%lu:%lu]\n",
                start_tv.tv_sec, start_tv.tv_usec,
                end_tv.tv_sec, end_tv.tv_usec,
                diff_tv.tv_sec, diff_tv.tv_usec);

    }
    if (i == REPEAT_COUNT)
    {
        printf("sum[%lu:%lu] count[%d]\n",
                sum_tv.tv_sec, sum_tv.tv_usec, count);
        printf("avg[%lu:%lu]\n",
                sum_tv.tv_sec / count, (sum_tv.tv_sec % count * 1000000 + sum_tv.tv_usec) / count);
    }

    close(sock);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s client|server\n", argv[0]);
        return -1;
    }
    if (strcmp(argv[1], "client") == 0)
    {
        printf("client mode..\n");
        client_main();
    }
    else if (strcmp(argv[1], "server") == 0)
    {
        printf("server mode..\n");
        server_main();
    }
    else
    {
        printf("Usage: %s client|server\n", argv[0]);
        return -1;
    }
    return 0;
}


