#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/queue.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

/* Fallback macro for glibc sys/queue.h if SLIST_FOREACH_SAFE is missing */
#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = SLIST_FIRST((head));				\
	    (var) && ((tvar) = SLIST_NEXT((var), field), 1);		\
	    (var) = (tvar))
#endif

// Global flags and synchronization
volatile sig_atomic_t caught_sig = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_fd = -1;

// Node structure for SLIST linked list tracking threads
struct thread_node {
    pthread_t thread_id;
    int client_fd;
    int completed;
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(slisthead, thread_node) head = SLIST_HEAD_INITIALIZER(head);

// Signal handler for SIGINT and SIGTERM
static void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        caught_sig = 1;
        if (server_fd != -1) {
            close(server_fd); // Unblock accept()
        }
    }
}

// Thread function for appending 10-second RFC 2822 timestamps
void* timestamp_thread_func(void* arg) {
    (void)arg;
    while (!caught_sig) {
        sleep(10);
        if (caught_sig) break;

        time_t rawtime;
        struct tm *info;
        char time_str[200];
        
        time(&rawtime);
        info = localtime(&rawtime);
        
        // Format: timestamp:time\n (RFC 2822 compliant strftime)
        size_t len = strftime(time_str, sizeof(time_str), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", info);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATA_FILE, "a");
        if (fp != NULL) {
            fputs(time_str, fp);
            fclose(fp);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

// Thread function handling individual client connection sockets
void* worker_thread_func(void* arg) {
    struct thread_node *node = (struct thread_node*)arg;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    // Receive data packet and append to file under mutex lock
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(DATA_FILE, "a+");
    if (fp != NULL) {
        while ((bytes_read = recv(node->client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes_read] = '\0';
            fputs(buffer, fp);
            if (strchr(buffer, '\n') != NULL) {
                break; // End of packet
            }
        }
        fclose(fp);
    }
    
    // Read whole file contents and echo back to client
    fp = fopen(DATA_FILE, "r");
    if (fp != NULL) {
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            send(node->client_fd, buffer, bytes_read, 0);
        }
        fclose(fp);
    }
    pthread_mutex_unlock(&file_mutex);

    close(node->client_fd);
    node->completed = 1;
    return NULL;
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Register signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Setup socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return -1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        return -1;
    }

    if (daemon_mode) {
        if (daemon(0, 0) == -1) {
            perror("daemon failed");
            return -1;
    }

    // Spawn 10-second timer thread
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timestamp_thread_func, NULL);

    // Accept loop
    while (!caught_sig) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);

        if (client_fd < 0) {
            if (caught_sig) break;
            continue;
        }

        struct thread_node *node = malloc(sizeof(struct thread_node));
        node->client_fd = client_fd;
        node->completed = 0;

        SLIST_INSERT_HEAD(&head, node, entries);
        pthread_create(&node->thread_id, NULL, worker_thread_func, node);

        // Join completed threads
        struct thread_node *tmp1, *tmp2;
        SLIST_FOREACH_SAFE(tmp1, &head, entries, tmp2) {
            if (tmp1->completed) {
                pthread_join(tmp1->thread_id, NULL);
                SLIST_REMOVE(&head, tmp1, thread_node, entries);
                free(tmp1);
            }
        }
    }

    // Clean up
    pthread_join(timer_tid, NULL);

    struct thread_node *node;
    while (!SLIST_EMPTY(&head)) {
        node = SLIST_FIRST(&head);
        pthread_join(node->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(node);
    }

    pthread_mutex_destroy(&file_mutex);
    unlink(DATA_FILE);
    closelog();

    return 0;
}
