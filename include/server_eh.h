#ifndef server_eh
#define server_eh 1

#include <stdlib.h>

// http request components

struct http_header {
    char *name, *value;
    struct http_header *next;
};

static inline struct http_header *new_http_header() {
    struct http_header *header = malloc(sizeof(struct http_header));
    header->name = NULL;
    header->value = NULL;
    header->next = NULL;
    return header;
}

static inline void delete_http_header(struct http_header *header) {
    if (header->name != NULL) free(header->name);
    if (header->value != NULL) free(header->value);
    free(header);
}

struct http_request {
    char method, *url, *body;
    unsigned int flags;
    unsigned short http_major, http_minor;
    struct http_header *headers;
};

#define F_HREQ_KEEPALIVE 0x01

static inline struct http_request *new_http_request() {
    struct http_request *request = malloc(sizeof(struct http_request));
    request->headers = NULL;
    request->url = NULL;
    request->body = NULL;
    request->flags = 0;
    request->http_major = 0;
    request->http_minor = 0;
    return request;
}

static inline void delete_http_request(struct http_request *request) {
    if (request->url != NULL) free(request->url);
    if (request->body != NULL) free(request->body);
    struct http_header *header = request->headers;
    while (header != NULL) {
        struct http_header *to_delete = header;
        header = header->next;
        delete_http_header(to_delete);
    }
    free(request);
}

static inline struct http_header *add_http_header(struct http_request *request) {
    struct http_header *header = request->headers;
    while (header != NULL) {
        if (header->next == NULL) {
            header->next = new_http_header();
            return header->next;
        }
        header = header->next;
    }
    request->headers = new_http_header();
    return request->headers;
}

// server library interface

#include <ev.h>
#include <sys/socket.h>

struct http_server {
    struct ev_loop *loop;
    struct sockaddr_in *listen_addr;
    void (*handle_request)(struct http_request *request, int fd);
    struct ev_io *ev_accept;
};

int http_server_loop(struct http_server *server);

#endif
