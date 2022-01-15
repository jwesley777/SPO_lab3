#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include "server.h"
#include "config_worker.h"
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <regex.h>

#define BUFSIZE 1024
const char *DEFAULT_SERVER_PORT = "8085";
char *config_file = "server_config.ini";

atomic_bool stop_socket_thread = false;

extern char **environ; /* the environment */

int parseUri(int client_socket_fd, char filename[1024], char uri[1024], char cgiargs[1024]);

void response_static_file(int client_socket_fd, char filename[BUFSIZE], off_t size_file);

int check_file(int client_socket_fd, char filename[1024], struct stat *sbuf);

void *check_stop_server(void *data);

char *getExt(int client_socket_fd, const char *file_name);

/*
 * error - wrapper for perror used for bad syscalls
 */
void error(char *msg) {
    perror(msg);
    exit(1);
}

/*
 * cerror - returns an error message to the client
 */
void cerror(int client_socket_fd, char *cause, char *errno,
            char *shortmsg, char *longmsg) {
    FILE *stream;          /* client socket descriptor */
    stream = fdopen(client_socket_fd, "r+");

    fprintf(stream, "HTTP/1.1 %s %s\n", errno, shortmsg);
    fprintf(stream, "Content-type: text/html\n");
    fprintf(stream, "\n");
    fprintf(stream, "<html><title>Lab3 Error</title>");
    fprintf(stream, "<body bgcolor=""ffffff"">\n");
    fprintf(stream, "%s: %s\n", errno, shortmsg);
    fprintf(stream, "<p>%s: %s\n", longmsg, cause);
    fprintf(stream, "<hr><em>The Lab3 Web server</em>\n");
    fclose(stream);
    close(client_socket_fd);
}

void execute_server(char *work_dir, char *server_port) {
    int port_number;            /* port to listen on */
    port_number = server_port? atoi(server_port): atoi(DEFAULT_SERVER_PORT);
    printf("port number: %d\n", port_number);

    start_work_with_config_file(work_dir, config_file);

    // handle stop server on 'c' input in another thread
    pthread_t thread;
    pthread_create(&thread, NULL, check_stop_server, NULL);


    /* variables for connection management */
    int parent_fd;          /* parent socket */
    struct sockaddr_in server_addr; /* server's addr */
    struct sockaddr_in client_addr; /* client addr */

    /* variables for connection I/O */
    char buf[BUFSIZE];     /* message buffer */
    char method[BUFSIZE];  /* request method */
    char uri[BUFSIZE];     /* request uri */
    char version[BUFSIZE]; /* request method */
    char filename[BUFSIZE];/* path derived from uri */
    int is_static;         /* static request? */
    char cgi_args[BUFSIZE]; /* cgi argument list */
    struct stat sbuf;      /* file status */
    int pid;               /* process id from fork */
    int wait_status;       /* status from wait */

    /* open socket descriptor */
    parent_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (parent_fd < 0) {
        error("ERROR opening socket");
        return;
    }
    printf("[OK]: Socket started successfully \n");

    /* bind port to socket */
    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short) port_number);
    if (bind(parent_fd, (struct sockaddr *) &server_addr,
             sizeof(server_addr)) < 0)
        error("ERROR on binding");
    printf("[OK]: Successfully binded \n");

    /* get us ready to accept connection requests */
    if (listen(parent_fd, 5) < 0) /* allow 5 requests to queue up */
        error("ERROR on listen");


    printf("[INFO]: Try to open 'http://127.0.0.1:%d' in your browser\n", port_number);

    /*
     * main loop: wait for a connection request, parse HTTP,
     * serve requested content, close connection.
     */
    while (1) {
        memset(&buf, 0, BUFSIZE);
        if (stop_socket_thread) {
            printf("[OK]: Stop server \n");
            exit(1);
        }

        int client_socket_fd;
        int client_address_len;
        client_address_len = sizeof(client_addr);

        /* open the child socket descriptor*/
        if ((client_socket_fd = accept(parent_fd, (struct sockaddr *) &client_addr, &client_address_len)) < 0)
            error("ERROR on fdopen");

        /* get the HTTP request line */
        recv(client_socket_fd, buf, BUFSIZE, 0);
        FILE *log_file = fopen("./log.txt", "w");
        fclose(log_file);
        sscanf(buf, "%s %s %s\n", method, uri, version);
        char *q = strstr(buf, "Content-Length:");
        char *body;
        if (q != NULL) {
            body = strstr(q, "\r\n\r\n") + 4;
            q = NULL;
        }

        /* parse the uri */
        strcpy(filename, work_dir);
        is_static = parseUri(client_socket_fd, filename, uri, cgi_args);
        printf("is_static: %d\n", is_static);

        /* make sure the file exists */
        if (check_file(client_socket_fd, filename, &sbuf) < 0) {
            continue;
        }
        printf("file exists\n");

        /* serve static content */
        if (is_static) {
            response_static_file(client_socket_fd, filename, sbuf.st_size);
        } else {
            /* a real server would set other CGI environ vars as well*/
            setenv("QUERY_STRING", cgi_args, 1);
            setenv("REQUEST_METHOD", method, 1);
            setenv("SCRIPT_FILENAME", filename, 1);
            setenv("REDIRECT_STATUS", "200", 1);
            if (body != NULL) {
                setenv("BODY", body, 1);
                body = NULL;
            } else {
                setenv("BODY", "", 1);
            }

            /* print first part of response header */
            sprintf(buf, "HTTP/1.1 200 Ok\r\n");
            send(client_socket_fd, buf, strlen(buf), 0);
            sprintf(buf, "Content-Type: text/html; charset=utf-8\r\n");
            send(client_socket_fd, buf, strlen(buf), 0);
            sprintf(buf, "Server: Lab3 Server\n");
            send(client_socket_fd, buf, strlen(buf), 0);

            /* create and run the child CGI process so that all child
               output to stdout and stderr goes back to the client via the
               client_socket_fd */
            pid = fork();
            //pid = 0;
            if (pid < 0) {
                perror("ERROR in fork");
                exit(1);
            } else if (pid > 0) { /* parent process */
                wait(&wait_status);
            } else { /* child  process*/
                close(0); /* close stdin */
                dup2(client_socket_fd, 1); /* map socket to stdout */
                dup2(client_socket_fd, 2); /* map socket to stderr */

                char *executor = get_value(getExt(client_socket_fd, filename));
                if (executor == NULL) {
                    cerror(client_socket_fd, filename, "500", "Can't execute for this cgi",
                           "Can't execute");
                    continue;
                };
                char *f = filename;
                char *arg[] = {executor, filename, (char *) 0};
//                printf("%d", execve(value, arg, environ));
                if (execve(executor, arg, environ) < 0) {
                    perror("ERROR in execve");
                }

            }
        }
        printf("Sent %s %ld bytes\n\n", filename, sbuf.st_size);
        close(client_socket_fd);
    }
}

char *getExt(int client_socket_fd, const char *file_name) {
    char *e = strrchr(file_name, '.');
    if (e == NULL) {
        cerror(client_socket_fd, "500", "Oops...smth went wrong", "Try late", "Oops...smth went wrong");
    }
    return e;
}

int check_file(int client_socket_fd, char filename[1024], struct stat *sbuf) {
    if (stat(filename, sbuf) < 0) {
        cerror(client_socket_fd, filename, "404", "Not found",
               "Server couldn't find this file");
        return -1;
    }

    return 1;
}

int parseUri(int client_socket_fd, char filename[1024], char uri[1024], char cgiargs[1024]) {
    char *filter = get_value("filter");
    if (filter != NULL) {
        regex_t regex;
        int return_value;
        return_value = regcomp(&regex, filter, 0);
        if (return_value == 0) {
            return_value = regexec(&regex, uri, 0, NULL, 0);
            if (return_value == 0) {
                cerror(client_socket_fd, "403", "Forbidden", "Forbidden", "Not enough permission");
            }
        }
    }
    printf("uri: %s\n", uri);
    if (!strstr(uri, "cgi-bin")) { /* static content */
        strcpy(cgiargs, "");
        strcat(filename, strstr(uri, "/") + 1);
        if (uri[strlen(uri) - 1] == '/')
            strcat(filename, "index.html");
        return 1;
    } else { /* dynamic content */
        char *p;/* temporary pointer */
        p = index(uri, '?');
        if (p) {
            strcpy(cgiargs, p + 1);
            *p = '\0';
        } else {
            strcpy(cgiargs, "");
        }
        strcat(filename, uri + 1);
        return 0;
    }
}

void response_static_file(int client_socket_fd, char filename[BUFSIZE], off_t size_file) {
    int fd;                /* static content filedes */
    char buf[BUFSIZE];     /* message buffer */

    /* parse mime type */
    char *ext = getExt(client_socket_fd, filename);
    char *mime_type = get_value(ext + 1);
    if (mime_type == NULL) {
        mime_type = "text/plain; charset=utf-8";
    }

    /* print response header */
    sprintf(buf, "HTTP/1.1 200 OK\r\n");
    strcat(buf, "Host: 127.0.0.1\r\n");
    char *new = malloc(128);
    strcat(buf, "Server: Tiny Web Server\r\n");
    sprintf(new, "Content-length: %ld\r\n", size_file);
    strcat(buf, new);
    sprintf(new, "Content-type: %s\r\n\r\n", mime_type);
    strcat(buf, new);
    send(client_socket_fd, buf, strlen(buf), 0);

    /* Use mmap to return arbitrary-sized response body */
    fd = open(filename, O_RDONLY);
    ssize_t offset = 0;
    ssize_t rbytes = 0;
    while (1) {
        rbytes = pread(fd, buf, BUFSIZE, offset);
        if (rbytes <= 0) {
            break;
        }
        send(client_socket_fd, buf, rbytes, 0);
        offset += rbytes;
    }

    close(fd);
    close(client_socket_fd);
}

void *check_stop_server(void *data) {
    while (1) {
        if (getchar() == (int) 'c') {
            stop_socket_thread = true;
            return NULL;
        }
    }
}
