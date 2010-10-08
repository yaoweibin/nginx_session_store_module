
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    u_short start;
    u_short length;
} ngx_http_session_store_record_t;

typedef struct {
    ngx_rbtree_node_t               node;
    ngx_queue_t                     queue;
    ngx_uint_t                      state;
    time_t                          expire;
    u_short                         key_len;
    ngx_http_session_store_record_t record[8];
    u_char                          data[1];
} ngx_http_session_store_node_t;

typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
    ngx_queue_t                   queue;
} ngx_http_session_store_shctx_t;

typedef struct {
    ngx_http_session_store_shctx_t  *sh;
    ngx_slab_pool_t                 *shpool;
    ngx_int_t                        index;
    ngx_str_t                        var;
} ngx_http_session_store_ctx_t;

typedef struct {
    time_t          expire;

    ngx_shm_zone_t *store_shm_zone;
    ngx_uint_t      store_index[8];

    ngx_shm_zone_t *get_shm_zone;
    ngx_uint_t      get_index[8]; 
} ngx_http_session_store_loc_conf_t;

typedef struct {
    ngx_str_t record[8];
} ngx_http_session_store_variable_ctx_t;

static ngx_int_t ngx_http_session_store_by_key(ngx_http_request_t *r);
static ngx_int_t ngx_http_session_get_by_key(ngx_http_request_t *r);

static void ngx_http_session_store_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_int_t ngx_http_session_store_lookup(ngx_http_session_store_ctx_t *ctx, 
        ngx_uint_t hash, u_char *data, size_t len, 
        ngx_http_session_store_node_t **ssnp);
static void ngx_http_session_store_delete(ngx_http_session_store_ctx_t *ctx, 
        ngx_http_session_store_node_t  *ssn);
static void ngx_http_session_store_expire(ngx_http_session_store_ctx_t *ctx,
        ngx_uint_t n);

static ngx_int_t ngx_http_session_store_init(ngx_conf_t *cf);

static void * ngx_http_session_store_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_session_store_merge_loc_conf(ngx_conf_t *cf, 
        void *parent, void *child);

static char *ngx_http_session_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_session_store(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_session_get(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_session_store_commands[] = {

    { ngx_string("session_zone"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_session_zone,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("session_store"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_session_store,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("session_get"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_session_get,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_session_store_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_session_store_init,           /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_session_store_create_loc_conf, /* create location configuration */
    ngx_http_session_store_merge_loc_conf   /* merge location configuration */
};


ngx_module_t  ngx_http_session_store_module = {
    NGX_MODULE_V1,
    &ngx_http_session_store_module_ctx,    /* module context */
    ngx_http_session_store_commands,       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t 
ngx_http_session_store_handler(ngx_http_request_t *r)
{
    ngx_int_t                          rc;
    ngx_http_session_store_loc_conf_t *sslcf;

    sslcf = ngx_http_get_module_loc_conf(r, ngx_http_session_store_module);

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log,
            0, "session handler: store_shm_zone=%p, store_index=%d, get_shm_zone=%p, get_index=%d",
            sslcf->store_shm_zone, sslcf->store_index[0], 
            sslcf->get_shm_zone, sslcf->get_index[0]);

    if (sslcf->store_shm_zone != NULL && sslcf->store_index[0] != NGX_CONF_UNSET_UINT) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "session store");

        rc = ngx_http_session_store_by_key(r);
        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (sslcf->get_shm_zone != NULL && sslcf->get_index[0] != NGX_CONF_UNSET_UINT) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "session get");

        rc = ngx_http_session_get_by_key(r);
        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t 
ngx_http_session_store_by_key(ngx_http_request_t *r)
{
    u_char                             *last;
    size_t                              n, len;
    uint32_t                            hash;
    ngx_int_t                           rc;
    ngx_uint_t                          i, index;
    ngx_rbtree_node_t                  *node;
    ngx_http_variable_value_t          *v, *vv;
    ngx_http_session_store_ctx_t       *ctx;
    ngx_http_session_store_node_t      *ssn;
    ngx_http_session_store_loc_conf_t  *sslcf;

    sslcf = ngx_http_get_module_loc_conf(r, ngx_http_session_store_module);

    if (sslcf->store_shm_zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = sslcf->store_shm_zone->data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "session ctx: %p", ctx);

    v = ngx_http_get_indexed_variable(r, ctx->index);

    if (v == NULL || v->not_found) {
        return NGX_DECLINED;
    }

    len = v->len;

    if (len == 0) {
        return NGX_DECLINED;
    }

    if (len > 65535) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" variable "
                      "is more than 65535 bytes: \"%v\"",
                      &ctx->var, v);
        return NGX_ERROR;
    }

    hash = ngx_crc32_short(v->data, len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_http_session_store_expire(ctx, 1);

    rc = ngx_http_session_store_lookup(ctx, hash, v->data, len, &ssn);

    /* Find, delete the old one */

    if (ssn) {
        ngx_http_session_store_delete(ctx, ssn);
    };

    /* Not find */

    n = offsetof(ngx_rbtree_node_t, color)
        + offsetof(ngx_http_session_store_node_t, data)
        + len;

    for (i = 0; i < 8; i++) {
        index = sslcf->store_index[i];

        if (index == NGX_CONF_UNSET_UINT) {
            break;
        }

        vv = ngx_http_get_indexed_variable(r, index);

        if (vv == NULL || vv->not_found) {
            break;
        }

        n += vv->len;
    }

    node = ngx_slab_alloc_locked(ctx->shpool, n);
    if (node == NULL) {
        ngx_http_session_store_expire(ctx, 0);

        node = ngx_slab_alloc_locked(ctx->shpool, n);
        if (node == NULL) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, 
                    "not enough share memory for zone \"%V\"", 
                    &sslcf->store_shm_zone->shm.name);
            return NGX_ERROR;
        }
    }

    ssn = (ngx_http_session_store_node_t *) &node->color;

    node->key = hash;
    ssn->key_len = (u_char) len;
    ssn->expire = ngx_time() + sslcf->expire;

    last = ssn->data;

    ngx_memcpy(last, v->data, len);
    last += len;

    for (i = 0; i < 8; i++) {
        index = sslcf->store_index[i];

        if (index == NGX_CONF_UNSET_UINT) {
            break;
        }

        vv = ngx_http_get_indexed_variable(r, index);

        if (vv == NULL || vv->not_found) {
            break;
        }

        ssn->record[i].start = last - ssn->data;
        ssn->record[i].length = vv->len;

        ngx_memcpy(last, vv->data, vv->len);
        last += vv->len;
    }

    ngx_rbtree_insert(&ctx->sh->rbtree, node);

    ngx_queue_insert_head(&ctx->sh->queue, &ssn->queue);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}


ngx_int_t
ngx_http_session_get_by_key(ngx_http_request_t *r)
{
    size_t                             len;
    uint32_t                           hash;
    ngx_int_t                          rc;
    ngx_uint_t                         i, index;
    ngx_http_variable_value_t         *v, *vv;
    ngx_http_session_store_ctx_t      *ctx;
    ngx_http_session_store_node_t     *ssn;
    ngx_http_session_store_loc_conf_t *sslcf;
    ngx_http_session_store_variable_ctx_t *variable_ctx;

    sslcf = ngx_http_get_module_loc_conf(r, ngx_http_session_store_module);

    if (sslcf->get_shm_zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = sslcf->get_shm_zone->data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "session ctx: %p", ctx);

    v = ngx_http_get_indexed_variable(r, ctx->index);

    if (v == NULL || v->not_found) {
        return NGX_DECLINED;
    }

    len = v->len;

    if (len == 0) {
        return NGX_DECLINED;
    }

    if (len > 65535) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" variable "
                      "is more than 65535 bytes: \"%v\"",
                      &ctx->var, v);
        return NGX_DECLINED;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "find session: %*s", len, v->data);

    hash = ngx_crc32_short(v->data, len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_http_session_store_expire(ctx, 1);

    rc = ngx_http_session_store_lookup(ctx, hash, v->data, len, &ssn);

    if (ssn == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                "not find session");

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        return NGX_OK;
    }

    ngx_queue_remove(&ssn->queue);
    ngx_queue_insert_head(&ctx->sh->queue, &ssn->queue);

    for (i = 0; i < 8; i++) {
        index = sslcf->get_index[i];

        if (index == NGX_CONF_UNSET_UINT) {
            break;
        }

        vv = ngx_http_get_indexed_variable(r, index);

        if (vv == NULL) {
            break;
        }

        variable_ctx = ngx_http_get_module_ctx(r, ngx_http_session_store_module);
        if (variable_ctx == NULL) {
            variable_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_session_store_variable_ctx_t));
            if (variable_ctx == NULL) {
                return NGX_ERROR;
            }

            ngx_http_set_ctx(r, variable_ctx, ngx_http_session_store_module);
        }

        variable_ctx->record[i].data = ssn->data + ssn->record[i].start;
        variable_ctx->record[i].len = ssn->record[i].length;

        vv->len = variable_ctx->record[i].len;
        vv->valid = 1;
        vv->no_cacheable = 0;
        vv->not_found = 0;
        vv->data = variable_ctx->record[i].data;

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                "value[%d] = %V", i, &variable_ctx->record[i]);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}


static ngx_int_t 
ngx_http_session_store_get_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    v->not_found = 1;
    return NGX_OK;
}


static void
ngx_http_session_store_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t               **p;
    ngx_http_session_store_node_t   *ssn, *ssnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            ssn = (ngx_http_session_store_node_t *) &node->color;
            ssnt = (ngx_http_session_store_node_t *) &temp->color;

            p = (ngx_memn2cmp(ssn->data, ssnt->data, ssn->key_len, ssnt->key_len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_http_session_store_lookup(ngx_http_session_store_ctx_t *ctx, ngx_uint_t hash,
    u_char *data, size_t len, ngx_http_session_store_node_t **ssnp)
{
    time_t                          now;
    ngx_int_t                       rc;
    ngx_rbtree_node_t              *node, *sentinel;
    ngx_http_session_store_node_t  *ssn;


    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        do {
            ssn = (ngx_http_session_store_node_t *) &node->color;

            rc = ngx_memn2cmp(data, ssn->data, len, (size_t) ssn->key_len);

            if (rc == 0) {

                now = ngx_time();

                /*TODO*/

                *ssnp = ssn;

                return NGX_OK;
            }

            node = (rc < 0) ? node->left : node->right;

        } while (node != sentinel && hash == node->key);

        break;
    }

    *ssnp = NULL;

    return NGX_OK;
}


static void
ngx_http_session_store_delete(ngx_http_session_store_ctx_t *ctx, 
        ngx_http_session_store_node_t  *ssn)
{
    ngx_rbtree_node_t              *node;

    ngx_queue_remove(&ssn->queue);

    node = (ngx_rbtree_node_t *)
        ((u_char *) ssn - offsetof(ngx_rbtree_node_t, color));

    ngx_rbtree_delete(&ctx->sh->rbtree, node);

    ngx_slab_free_locked(ctx->shpool, node);
}


static void
ngx_http_session_store_expire(ngx_http_session_store_ctx_t *ctx, ngx_uint_t n)
{
    time_t                          now;
    ngx_queue_t                    *q;
    ngx_rbtree_node_t              *node;
    ngx_http_session_store_node_t  *ssn;

    now = ngx_time();

    /*
     * n == 1 deletes one or two entries
     * n == 0 deletes oldest entry by force
     *        and one or two entries
     */

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        ssn = ngx_queue_data(q, ngx_http_session_store_node_t, queue);

        if (n++ != 0) {
            if (now < ssn->expire) {
                return;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) ssn - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
    }
}


static ngx_int_t
ngx_http_session_store_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t                         len;
    ngx_http_session_store_ctx_t  *ctx;
    ngx_http_session_store_ctx_t  *octx = data;

    ctx = shm_zone->data;

    if (octx) {
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_session_store_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_session_store_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in session store zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in session store zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static char *
ngx_http_session_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                        *p;
    size_t                         size;
    ngx_str_t                     *value, name, s;
    ngx_uint_t                     i;
    ngx_shm_zone_t                *shm_zone;
    ngx_http_session_store_ctx_t  *ctx;

    value = cf->args->elts;

    ctx = NULL;
    size = 0;
    name.len = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p) {
                *p = '\0';

                name.len = p - name.data;

                p++;

                s.len = value[i].data + value[i].len - p;
                s.data = p;

                size = ngx_parse_size(&s);
                if (size > 8191) {
                    continue;
                }
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid zone size \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (value[i].data[0] == '$') {

            value[i].len--;
            value[i].data++;

            ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_session_store_ctx_t));
            if (ctx == NULL) {
                return NGX_CONF_ERROR;
            }

            ctx->index = ngx_http_get_variable_index(cf, &value[i]);
            if (ctx->index == NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            ctx->var = value[i];

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0 || size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (ctx == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no variable is defined for session_zone \"%V\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_session_store_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_session_store_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_http_session_store(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_session_store_loc_conf_t  *sslcf = conf;

    ngx_str_t   *value, s;
    ngx_uint_t   i, n;

    if (sslcf->store_shm_zone) {
        return "is duplicate";
    }

    value = cf->args->elts;
    n = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            sslcf->store_shm_zone = ngx_shared_memory_add(cf, &s, 0,
                    &ngx_http_session_store_module);
            if (sslcf->store_shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "expire=", 7) == 0) {

            s.len = value[i].len - 7;
            s.data = value[i].data + 7;

            sslcf->expire = ngx_parse_time(&s, 1);
            if (sslcf->expire == (time_t) NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (value[i].data[0] == '$') {

            value[i].len--;
            value[i].data++;

            sslcf->store_index[n] = ngx_http_get_variable_index(cf, &value[i]);
            if (sslcf->store_index[n] == (ngx_uint_t) NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            n++;

            continue;
        }


        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (sslcf->store_shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (sslcf->store_shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "unknown session_store_zone \"%V\"",
                &sslcf->store_shm_zone->shm.name);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_session_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_session_store_loc_conf_t  *sslcf = conf;

    ngx_str_t                 *value, s;
    ngx_uint_t                 i, n;
    ngx_http_variable_t       *v;

    if (sslcf->get_shm_zone) {
        return "is duplicate";
    }

    value = cf->args->elts;
    n = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            sslcf->get_shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                                   &ngx_http_session_store_module);
            if (sslcf->get_shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (value[i].data[0] == '$') {

            value[i].len--;
            value[i].data++;

            v = ngx_http_add_variable(cf, &value[i], NGX_HTTP_VAR_CHANGEABLE);
            if (v == NULL) {
                return NGX_CONF_ERROR;
            }

            v->get_handler = ngx_http_session_store_get_variable;
            v->data = n;

            sslcf->get_index[n] = ngx_http_get_variable_index(cf, &value[i]);
            if (sslcf->get_index[n] == (ngx_uint_t) NGX_ERROR) {
                return NGX_CONF_ERROR;
            }

            n++;

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (sslcf->get_shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (sslcf->get_shm_zone->data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "unknown session_get_zone \"%V\"",
                &sslcf->get_shm_zone->shm.name);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_session_store_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_session_store_handler;

    return NGX_OK;
}


static void *
ngx_http_session_store_create_loc_conf(ngx_conf_t *cf)
{
    size_t                              i;
    ngx_http_session_store_loc_conf_t  *sslcf;

    sslcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_session_store_loc_conf_t));
    if (sslcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     sslcf->store_shm_zone =  NULL;
     *     sslcf->store_index[8] =  {0};
     *     sslcf->get_shm_zone   =  NULL;
     *     sslcf->get_index[8]   =  {0};
     */

    sslcf->expire = NGX_CONF_UNSET;

    for (i = 0; i < 8; i++) {
        sslcf->store_index[i] = NGX_CONF_UNSET_UINT;
    }

    for (i = 0; i < 8; i++) {
        sslcf->get_index[i] = NGX_CONF_UNSET_UINT;
    }

    return sslcf;
}


static char *
ngx_http_session_store_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_session_store_loc_conf_t *prev = parent;
    ngx_http_session_store_loc_conf_t *conf = child;

    ngx_conf_merge_sec_value(conf->expire, prev->expire, 60);

    return NGX_CONF_OK;
}
