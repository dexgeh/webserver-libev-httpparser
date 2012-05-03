#include "server_eh.h"
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

// utils

static inline int setnonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return flags;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) return -1;
    return 0;
}

#define REQUEST_BUFFER_SIZE 2048

#define alloc_cpy(dest, src, len) \
    dest = malloc(len + 1);\
    memcpy(dest, src, len);\
    dest[len] = '\0';

// http_parser callback and settings

int null_cb(http_parser *parser) { return 0; }

int url_cb(http_parser *parser, const char *buf, size_t len) {
    struct http_request *request = (struct http_request *) parser->data;
    request->method = parser->method;
    request->http_major = parser->http_major;
    request->http_minor = parser->http_minor;
    alloc_cpy(request->url, buf, len)
    return 0;
}

int header_field_cb(http_parser *parser, const char *buf, size_t len) {
    struct http_request *request = (struct http_request *) parser->data;
    struct http_header *header = add_http_header(request);
    alloc_cpy(header->name, buf, len)
    return 0;
}

int header_value_cb(http_parser *parser, const char *buf, size_t len) {
    struct http_request *request = (struct http_request *) parser->data;
    struct http_header *header = request->headers;
    while (header->next != NULL) {
        header = header->next;
    }
    alloc_cpy(header->value, buf, len)
    return 0;
}

int body_cb(http_parser *parser, const char *buf, size_t len) {
    struct http_request *request = (struct http_request *) parser->data;
    alloc_cpy(request->body, buf, len)
    return 0;
}

static http_parser_settings parser_settings =
{
     .on_message_begin    = null_cb
    ,.on_message_complete = null_cb
    ,.on_headers_complete = null_cb
    ,.on_header_field     = header_field_cb
    ,.on_header_value     = header_value_cb
    ,.on_url              = url_cb
    ,.on_body             = body_cb
};

struct http_request *parse_request(char *request_data, int len) {
    http_parser *parser = malloc(sizeof(http_parser));
    http_parser_init(parser, HTTP_REQUEST);
    struct http_request *request = new_http_request();
    parser->data = request;
    int res = http_parser_execute(parser, &parser_settings, request_data, len);
    if (res == len) {
        if (http_should_keep_alive(parser)) {
            request->flags |= F_HREQ_KEEPALIVE;
        }
        free(parser);
        return request;
    }
    delete_http_request(request);
    free(parser);
    return NULL;
}

// libev callbacks and client structures

struct client {
    int fd;
    ev_io ev_accept;
    ev_io ev_read;
    ev_io ev_write;
    char *request_data;
    struct http_request *request;
    void (*handle_request)(struct http_request *request, int fd);
};

static void write_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    if (!(revents & EV_WRITE)) {
        ev_io_stop(EV_A_ w);
        return;
    }
    struct client *client =
        (struct client *)
        (((char *) w) - offsetof(struct client, ev_write));
    struct http_request *request = client->request;
    if (request == NULL) {
        write(client->fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 24);
        close(client->fd);
        free(client->request_data);
        free(client);
        ev_io_stop(EV_A_ w);
        return;
    }
    client->handle_request(request, client->fd);
    delete_http_request(request);
    free(client->request_data);
    free(client);
    ev_io_stop(EV_A_ w);
}

static void read_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    if (!(revents & EV_READ)) {
        ev_io_stop(EV_A_ w);
        return;
    }
    struct client *client =
        (struct client *)
        (((char *) w) - offsetof(struct client, ev_read));
    char *rbuff[REQUEST_BUFFER_SIZE + 1];
    int sum = 0, len = 0;
    client->request_data = NULL;
    do {
        len = read(client->fd, &rbuff, REQUEST_BUFFER_SIZE);
        sum += len;
        if (len < REQUEST_BUFFER_SIZE)
            rbuff[len] = '\0';
        if (client->request_data == NULL) {
            client->request_data = malloc(len+1);
            memcpy(client->request_data, rbuff, len);
        } else {
            client->request_data = realloc(client->request_data, sum + 1);
            memcpy(client->request_data + sum - len, rbuff, len);
        }
    } while (len == REQUEST_BUFFER_SIZE);
    client->request = NULL;
    client->request = parse_request(client->request_data, len);
    ev_io_stop(EV_A_ w);
    ev_io_init(&client->ev_write, write_cb, client->fd, EV_WRITE);
    ev_io_start(loop, &client->ev_write);
}

static void accept_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    struct client *main_client =
        (struct client *)
        (((char *) w) - offsetof(struct client, ev_accept));
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(struct sockaddr_in);
    int client_fd = accept(w->fd, (struct sockaddr *) &client_addr, &client_len);
    if (client_fd == -1) {
        return;
    }
    if (setnonblock(client_fd) < 0) {
        perror("failed to set client socket to nonblock");
        return;
    }
    struct client *client = malloc(sizeof(struct client));
    client->handle_request = main_client->handle_request;
    client->fd = client_fd;
    ev_io_init(&client->ev_read, read_cb, client->fd, EV_READ);
    ev_io_start(loop, &client->ev_read);
}

int http_server_loop(struct http_server *server) {
    server->loop = ev_default_loop(0);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("listen failed(socket)");
        return -1;
    }
    int reuseaddr_on = 1;
    if (setsockopt(
            listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuseaddr_on,
            sizeof(server->listen_addr)) == -1) {
        perror("setsockopt failed");
        return -1;
    }
    struct sockaddr *listen_addr = (struct sockaddr *) server->listen_addr;
    if (bind(listen_fd, listen_addr, sizeof(*listen_addr)) < 0) {
        perror("bind failed");
        return -1;
    }
    if (listen(listen_fd, 5) < 0) {
        perror("listen failed(listen)");
        return -1;
    }
    if (setnonblock(listen_fd) < 0) {
        perror("failed to set server socket to nonblock");
        return -1;
    }
    struct client *main_client = malloc(sizeof(struct client));
    main_client->handle_request = server->handle_request;
    ev_io_init(&main_client->ev_accept, accept_cb, listen_fd, EV_READ);
    ev_io_start(server->loop, &main_client->ev_accept);
    server->ev_accept = &main_client->ev_accept;
    ev_loop(server->loop, 0);
    return 0;
}
