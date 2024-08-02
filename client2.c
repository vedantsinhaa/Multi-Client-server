#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>

#define MAX_NAME_LEN 20
#define MAX_MSG_LEN 100
#define MAX_CLIENTS 10

void *read_thread(void *arg) {
    char *shm_ptr = (char *) arg;

    while (1) {
        char msg[MAX_MSG_LEN];
        fgets(msg, MAX_MSG_LEN, stdin);

        strcpy(shm_ptr, msg);
    }

    return NULL;
}

int main() {
    char name[MAX_NAME_LEN];
    printf("Enter your name: ");
    fgets(name, MAX_NAME_LEN, stdin);
    name[strlen(name) - 1] = '\0';

    char *key = NULL;
    int comm_fd = -1;
    char *shm_ptr = NULL;

    while (key == NULL) {
        printf("Registering with server...\n");

        comm_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (comm_fd == -1) {
            perror("socket");
            return 1;
        }

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(8000);

        if (connect(comm_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
            perror("connect");
            close(comm_fd);
            sleep(1);
            continue;
        }

        char msg[MAX_NAME_LEN + 5];
        sprintf(msg, "REG %s", name);

        if (send(comm_fd, msg, strlen(msg) + 1, 0) == -1) {
            perror("send");
            close(comm_fd);
            sleep(1);
            continue;
        }

        char reply[MAX_NAME_LEN + 5];
        if (recv(comm_fd, reply, MAX_NAME_LEN + 5, 0) == -1) {
            perror("recv");
            close(comm_fd);
            sleep(1);
            continue;
        }

        if (strncmp(reply, "ERR", 3) == 0) {
            printf("Error: Name already taken or server is full.\n");
            close(comm_fd);
            sleep(1);
            continue;
        }

        key = strdup(reply);
    }

    int shm_fd = shm_open(key, O_RDWR, 0600);
    if (shm_fd == -1) {
        perror("shm_open");
        free(key);
        return 1;
    }

    shm_ptr = mmap(NULL, MAX_MSG_LEN * MAX_CLIENTS, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        shm_unlink(key);
        free(key);
        return 1;
    }

    printf("Registration successful. Your key is %s.\n", key);

    pthread_t thread;
    if (pthread_create(&thread, NULL, read_thread, (void *) shm_ptr) != 0) {
        perror("pthread_create");
        munmap(shm_ptr, MAX_MSG_LEN * MAX_CLIENTS);
        shm_unlink(key);
        free(key);
        close(comm_fd);
        return 1;
    }

    while (1) {
        printf("Waiting for message...\n");

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(comm_fd, &readfds);

        int max_fd = comm_fd;

        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) == -1) {
            perror("select");
            pthread_cancel(thread);
            munmap(shm_ptr, MAX_MSG_LEN * MAX_CLIENTS);
            shm_unlink(key);
            free(key);
            return 1;
        }

        char msg[MAX_MSG_LEN];
        if (FD_ISSET(comm_fd, &readfds)) {
            if (recv(comm_fd, msg, MAX_MSG_LEN, 0) == -1) {
                perror("recv");
                pthread_cancel(thread);
                munmap(shm_ptr, MAX_MSG_LEN * MAX_CLIENTS);
                shm_unlink(key);
                free(key);
                return 1;
            }

            if (strcmp(msg, "STOP") == 0) {
                printf("Server has stopped. Disconnecting...\n");
                break;
            }

            printf("%s\n", msg);
        }
    }

    if (strlen(shm_ptr) > 0) {
        printf("%s\n", shm_ptr);
        memset(shm_ptr, 0, MAX_MSG_LEN);
    }

    pthread_cancel(thread);
    munmap(shm_ptr, MAX_MSG_LEN * MAX_CLIENTS);
    shm_unlink(key);
    free(key);
    close(comm_fd);
    return 0;
}
