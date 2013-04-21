typedef struct plugin_struct *plugin_t;

typedef struct {
    conf_t *conf;
    void *zs_in;
    void *zs_in_event;
    void *zs_req;
    void *zs_out;
    void *zs_out_event;
    void *zs_out_tree;
    long timeout;
    void *zs_plout; /* server side socket, but private to this plugin */
    pthread_t t;
    plugin_t plugin;
    server_t *srv;
    void *ctx;
} plugin_ctx_t;

typedef enum { MSG_REQUEST, MSG_RESPONSE, MSG_EVENT } msg_type_t;

struct plugin_struct {
    const char *name;
    void (*timeoutFn)(plugin_ctx_t *p);
    void (*recvFn)(plugin_ctx_t *p, zmsg_t **zmsg, msg_type_t type);
    void (*pollFn)(plugin_ctx_t *p);
    void (*initFn)(plugin_ctx_t *p);
    void (*finiFn)(plugin_ctx_t *p);
};

void plugin_init (conf_t *conf, server_t *srv);
void plugin_fini (conf_t *conf, server_t *srv);
void plugin_send (server_t *srv, conf_t *conf, zmsg_t **zmsg);
