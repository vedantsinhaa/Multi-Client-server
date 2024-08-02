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

typedef struct {
    char name[MAX_NAME_LEN];
    int comm_fd;
    int shm_fd;
    char *shm_ptr;
    pthread_t thread;
} client_t;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
client_t clients[MAX_CLIENTS];
int num_clients = 0;


void *client_thread(void *arg) {
    client_t *client = (client_t *) arg;

    while (1) {
        // Wait for a message
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client->comm_fd, &rfds);

        if (select(client->comm_fd + 1, &rfds, NULL, NULL, NULL) == -1) {
            perror("select");
            break;
        }

        if (FD_ISSET(client->comm_fd, &rfds)) {
            char msg[MAX_MSG_LEN];
            memset(msg, 0, MAX_MSG_LEN);

            int ret = read(client->comm_fd, msg, MAX_MSG_LEN - 1);
            if (ret == -1) {
                perror("read");
                break;
            }

            // Store the message in the shared memory region
            strcpy(client->shm_ptr + (MAX_MSG_LEN * (client - clients)), msg);
        }
    }

    // Cleanup
    pthread_mutex_lock(&mutex);

    close(client->comm_fd);
    munmap(client->shm_ptr, MAX_MSG_LEN * MAX_CLIENTS);
    shm_unlink(client->name);

    // Remove the client from the array
    for (int i = 0; i < num_clients; i++) {
        if (&clients[i] == client) {
            num_clients--;
            for (int j = i; j < num_clients; j++) {
                clients[j] = clients[j+1];
            }
            break;
        }
    }

    pthread_mutex_unlock(&mutex);

    return NULL;
}

char *get_key(char *name) {
    pthread_mutex_lock(&mutex);

    // Check if the name already exists
    for (int i = 0; i < num_clients; i++) {
        if (strcmp(clients[i].name, name) == 0) {
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
    }

    // Check if the maximum number of clients has been reached
    if (num_clients == MAX_CLIENTS) {
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Generate a new key
    char key[MAX_NAME_LEN + 5];
    sprintf(key, "key_%s", name);

    // Create the communication channel
    int comm_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (comm_fd == -1) {
        perror("socket");
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8001 + num_clients);

    if (bind(comm_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind");
        close(comm_fd);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    if (listen(comm_fd, 1) == -1) {
        perror("listen");
        close(comm_fd);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    // Create the shared memory region
    int shm_fd = shm_open(key, O_CREAT | O_RDWR, 0600);
    if (shm_fd == -1) {
        perror("shm_open");
        close(comm_fd);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    if (ftruncate(shm_fd, MAX_MSG_LEN * MAX_CLIENTS) == -1) {
        perror("ftruncate");
        close(comm_fd);
        shm_unlink(key);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    char *shm_ptr = mmap(NULL, MAX_MSG_LEN * MAX_CLIENTS, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        close(comm_fd);
        shm_unlink(key);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    strcpy(clients[num_clients].name, name);
    clients[num_clients].comm_fd = comm_fd;
    clients[num_clients].shm_fd = shm_fd;
    clients[num_clients].shm_ptr = shm_ptr;

    if (pthread_create(&clients[num_clients].thread, NULL, client_thread, (void *) &clients[num_clients]) != 0) {
        perror("pthread_create");
        close(comm_fd);
        shm_unlink(key);
        pthread_mutex_unlock(&mutex);
        return NULL;
    }

    num_clients++;

    pthread_mutex_unlock(&mutex);

    return  key;
}



int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8000);

    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server started on port %d\n", ntohs(addr.sin_port));

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        char name[MAX_NAME_LEN];
        memset(name, 0, MAX_NAME_LEN);

        int ret = read(client_fd, name, MAX_NAME_LEN - 1);
        if (ret == -1) {
            perror("read");
            close(client_fd);
            continue;
        }

        char *key = get_key(name);
        if (key == NULL) {
            printf("Client %s already exists or maximum number of clients reached\n", name);
            close(client_fd);
            continue;
        }

        printf("Client %s registered with key %s\n", name, key);

        if (write(client_fd, key, strlen(key)) == -1) {
            perror("write");
            close(client_fd);
            shm_unlink(key);
            continue;
        }

        close(client_fd);
    }

    return 0;
}

