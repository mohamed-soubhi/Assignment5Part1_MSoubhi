#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>

#define PORT        9000
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BUF_SIZE    1024
#define BACKLOG     10

static int  server_fd = -1;
static int  client_fd = -1;
static volatile sig_atomic_t g_running = 1;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */
static void setup_signals(void);
static int  create_server_socket(void);
static void handle_client(int fd, const char *ip);

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */
static void signal_handler(int signo)
{
    (void)signo;
    syslog(LOG_INFO, "Caught signal, exiting");
    g_running = 0;
    /* Wake accept() if blocked */
    if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Socket setup                                                        */
/* ------------------------------------------------------------------ */
static int create_server_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, "setsockopt: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/* Client handler: receive → file → send-back                         */
/* ------------------------------------------------------------------ */
static void handle_client(int fd, const char *ip)
{
    char   recv_buf[BUF_SIZE];
    char  *packet     = NULL;
    size_t packet_len = 0;
    ssize_t n;

    while ((n = recv(fd, recv_buf, sizeof(recv_buf), 0)) > 0) {
        /* Grow packet buffer */
        char *tmp = realloc(packet, packet_len + (size_t)n + 1);
        if (!tmp) {
            syslog(LOG_ERR, "realloc failed, dropping packet from %s", ip);
            free(packet);
            packet     = NULL;
            packet_len = 0;
            continue;
        }
        packet = tmp;
        memcpy(packet + packet_len, recv_buf, (size_t)n);
        packet_len += (size_t)n;
        packet[packet_len] = '\0';

        /* Process every complete line (newline-terminated packet) */
        char *nl;
        while ((nl = strchr(packet, '\n')) != NULL) {
            size_t line_len = (size_t)(nl - packet) + 1; /* include '\n' */

            /* Append line to data file */
            FILE *f = fopen(DATA_FILE, "a");
            if (!f) {
                syslog(LOG_ERR, "fopen(a) %s: %s", DATA_FILE, strerror(errno));
            } else {
                fwrite(packet, 1, line_len, f);
                fclose(f);

                /* Send entire file back to client */
                f = fopen(DATA_FILE, "r");
                if (f) {
                    char send_buf[BUF_SIZE];
                    size_t bytes;
                    while ((bytes = fread(send_buf, 1, sizeof(send_buf), f)) > 0)
                        send(fd, send_buf, bytes, 0);
                    fclose(f);
                } else {
                    syslog(LOG_ERR, "fopen(r) %s: %s", DATA_FILE, strerror(errno));
                }
            }

            /* Shift remaining bytes to front of packet buffer */
            size_t remaining = packet_len - line_len;
            memmove(packet, packet + line_len, remaining);
            packet_len = remaining;
            packet[packet_len] = '\0';
        }
    }

    free(packet);
}

/* ------------------------------------------------------------------ */
/* Daemonize: fork after bind so parent can report bind errors        */
/* ------------------------------------------------------------------ */
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid > 0)
        exit(EXIT_SUCCESS); /* parent exits cleanly */

    /* Child: become session leader, detach from terminal */
    if (setsid() < 0) {
        syslog(LOG_ERR, "setsid: %s", strerror(errno));
        return -1;
    }

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    chdir("/");
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int daemon_mode = 0;

    if (argc == 2 && strcmp(argv[1], "-d") == 0)
        daemon_mode = 1;

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    setup_signals();

    server_fd = create_server_socket();
    if (server_fd < 0) {
        closelog();
        return -1;
    }

    /* Fork only after successful bind — parent knows port is claimed */
    if (daemon_mode && daemonize() < 0) {
        close(server_fd);
        closelog();
        return -1;
    }

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || !g_running)
                break;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        syslog(LOG_INFO, "Accepted connection from %s", ip);

        handle_client(client_fd, ip);

        syslog(LOG_INFO, "Closed connection from %s", ip);
        close(client_fd);
        client_fd = -1;
    }

    /* Cleanup */
    if (server_fd != -1) close(server_fd);
    remove(DATA_FILE);
    closelog();
    return 0;
}
