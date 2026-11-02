/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NATIVE_MAX_ROUTES 64
#define NATIVE_MAX_PREFIX 1024

typedef struct {
    ngx_str_t id;
    ngx_str_t prefix;
    ngx_str_t plan;
    ngx_str_t index;
} native_route_t;

typedef struct {
    ngx_flag_t enabled;
    ngx_array_t routes;
    ngx_str_t backend;
} native_srv_conf_t;

typedef struct {
    native_route_t *route;
} native_ctx_t;

static void *native_create_srv_conf(ngx_conf_t *cf);
static char *native_add_route(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t native_postconfiguration(ngx_conf_t *cf);
static ngx_int_t native_variables(ngx_conf_t *cf);
static ngx_int_t native_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t native_preaccess(ngx_http_request_t *r);

static ngx_int_t
native_forward_uri(ngx_http_request_t *r)
{
    u_char *src, *dst, *buf, *end, *query, *write, *read;
    size_t len;

    query = ngx_strlchr(r->unparsed_uri.data,
                       r->unparsed_uri.data + r->unparsed_uri.len, '?');
    len = query ? (size_t) (query - r->unparsed_uri.data) : r->unparsed_uri.len;
    if (len == r->uri.len
        && ngx_memcmp(r->unparsed_uri.data, r->uri.data, len) == 0) {
        return NGX_OK;
    }
    buf = ngx_pnalloc(r->pool, len);
    if (buf == NULL) {
        return NGX_ERROR;
    }
    src = r->unparsed_uri.data;
    dst = buf;
    ngx_unescape_uri(&dst, &src, len, 0);
    end = dst;
    write = buf;
    for (read = buf; read < end; read++) {
        if (*read == '/' && write > buf && write[-1] == '/') {
            continue;
        }
        *write++ = *read;
    }
    /* Match APISIX's default: forward normalization beyond decode/slash merge. */
    if ((size_t) (write - buf) != r->uri.len
        || ngx_memcmp(buf, r->uri.data, r->uri.len) != 0) {
        r->valid_unparsed_uri = 0;
    }
    return NGX_OK;
}

static ngx_command_t native_commands[] = {
    { ngx_string("apisix_native"), NGX_HTTP_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot, NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(native_srv_conf_t, enabled), NULL },
    { ngx_string("apisix_native_backend"), NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(native_srv_conf_t, backend), NULL },
    { ngx_string("apisix_native_route"), NGX_HTTP_SRV_CONF|NGX_CONF_TAKE3,
      native_add_route, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL },
    ngx_null_command
};

static ngx_http_module_t native_module_ctx = {
    native_variables, native_postconfiguration,
    NULL, NULL, native_create_srv_conf, NULL, NULL, NULL
};

ngx_module_t ngx_http_apisix_native_module = {
    NGX_MODULE_V1, &native_module_ctx, native_commands, NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NGX_MODULE_V1_PADDING
};

static void *
native_create_srv_conf(ngx_conf_t *cf)
{
    native_srv_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }
    conf->enabled = NGX_CONF_UNSET;
    if (ngx_array_init(&conf->routes, cf->pool, 4,
                       sizeof(native_route_t)) != NGX_OK) {
        return NULL;
    }
    return conf;
}

static char *
native_add_route(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    native_srv_conf_t *scf = conf;
    ngx_str_t *args = cf->args->elts;
    native_route_t *routes = scf->routes.elts;
    native_route_t *route;
    ngx_uint_t i;
    u_char *index;

    if (scf->routes.nelts >= NATIVE_MAX_ROUTES || args[1].len == 0
        || args[1].len > 128 || args[2].len == 0
        || args[2].len > NATIVE_MAX_PREFIX || args[2].data[0] != '/'
        || args[2].data[args[2].len - 1] != '/'
        || args[3].len != 1 || args[3].data[0] < '0'
        || args[3].data[0] > '3') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid native route (id, /prefix/, plan 0..3)");
        return NGX_CONF_ERROR;
    }
    for (i = 0; i < scf->routes.nelts; i++) {
        if ((routes[i].id.len == args[1].len
             && ngx_memcmp(routes[i].id.data, args[1].data, args[1].len) == 0)
            || (routes[i].prefix.len == args[2].len
             && ngx_memcmp(routes[i].prefix.data, args[2].data,
                           args[2].len) == 0)) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "duplicate native route id or prefix");
            return NGX_CONF_ERROR;
        }
    }
    route = ngx_array_push(&scf->routes);
    index = ngx_pnalloc(cf->pool, NGX_INT_T_LEN);
    if (route == NULL || index == NULL) {
        return NGX_CONF_ERROR;
    }
    route->id = args[1];
    route->prefix = args[2];
    route->plan = args[3];
    route->index.data = index;
    route->index.len = ngx_sprintf(index, "%ui", scf->routes.nelts) - index;
    return NGX_CONF_OK;
}

static ngx_int_t
native_preaccess(ngx_http_request_t *r)
{
    native_srv_conf_t *scf;
    native_ctx_t *ctx;
    native_route_t *routes;
    ngx_uint_t i;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_apisix_native_module);
    if (scf->enabled != 1) {
        return NGX_DECLINED;
    }
    ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_native_module);
    if (ctx != NULL) {
        return NGX_DECLINED;
    }
    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_http_set_ctx(r, ctx, ngx_http_apisix_native_module);
    routes = scf->routes.elts;
    for (i = 0; i < scf->routes.nelts; i++) {
        if (r->uri.len >= routes[i].prefix.len
            && (ctx->route == NULL
                || routes[i].prefix.len > ctx->route->prefix.len)
            && ngx_memcmp(r->uri.data, routes[i].prefix.data,
                          routes[i].prefix.len) == 0) {
            ctx->route = &routes[i];
        }
    }
    if (ctx->route != NULL && native_forward_uri(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    return NGX_DECLINED;
}

static ngx_int_t
native_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    native_ctx_t *ctx;
    native_srv_conf_t *scf;
    ngx_str_t value = ngx_null_string;

    scf = ngx_http_get_module_srv_conf(r, ngx_http_apisix_native_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_native_module);
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;

    if (data == 4 && scf->enabled == 1) {
        ngx_str_set(&value, "1");  /* one global Prometheus log plan */
    } else if (ctx != NULL && ctx->route != NULL) {
        switch (data) {
        case 0: value = ctx->route->id; break;
        case 1: value = ctx->route->index; break;
        case 2: value = ctx->route->plan; break;
        case 3: value = scf->backend; break;
        }
    }
    v->data = value.data;
    v->len = value.len;
    return NGX_OK;
}

static ngx_int_t
native_variables(ngx_conf_t *cf)
{
    ngx_str_t names[] = {
        ngx_string("apisix_native_route_id"),
        ngx_string("apisix_native_route_index"),
        ngx_string("apisix_native_route_plan"),
        ngx_string("apisix_native_upstream"),
        ngx_string("apisix_native_global_plan")
    };
    ngx_http_variable_t *var;
    ngx_uint_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        var = ngx_http_add_variable(cf, &names[i], NGX_HTTP_VAR_NOCACHEABLE);
        if (var == NULL) {
            return NGX_ERROR;
        }
        var->get_handler = native_variable;
        var->data = i;
    }
    return NGX_OK;
}

static ngx_int_t
native_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_handler_pt *handler;
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (handler == NULL) {
        return NGX_ERROR;
    }
    *handler = native_preaccess;
    return NGX_OK;
}
