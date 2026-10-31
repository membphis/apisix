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
#ifndef APISIX_NATIVE_ENGINE_H
#define APISIX_NATIVE_ENGINE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <lua.h>
#include <lauxlib.h>
#include <ngx_http_lua_api.h>

typedef enum {
    AC_REWRITE, AC_ACCESS, AC_BEFORE_PROXY, AC_HEADER_FILTER,
    AC_BODY_FILTER, AC_DELAYED_BODY_FILTER, AC_LOG, AC_PHASES
} ac_phase_e;

typedef struct ac_expr_s ac_expr_t;
typedef struct ac_table_pool_s ac_table_pool_t;
typedef struct ac_rate_snapshot_s ac_rate_snapshot_t;
typedef struct ac_key_auth_conf_s ac_key_auth_conf_t;
typedef struct ac_key_auth_index_s ac_key_auth_index_t;

typedef struct {
    ngx_str_t name;
    lua_Number priority;
    int module_ref;
    int callbacks[AC_PHASES];
    unsigned prefer_route:1;
    unsigned auth:1;
} ac_plugin_t;

typedef struct {
    ac_plugin_t *plugin;
    int conf_ref;
    int pre_ref;
    int native_ref;
    ac_key_auth_conf_t *key_auth;
    lua_Number priority;
    ac_expr_t *expression;
    unsigned consumer_only:1;
} ac_call_t;

typedef struct { ac_call_t *call; ngx_flag_t matched; } ac_meta_match_t;

typedef struct {
    ngx_array_t calls;
    ngx_array_t phase_calls[AC_PHASES];
    ngx_flag_t indexed;
} ac_plan_t;

typedef struct ac_upstream_s ac_upstream_t;
typedef struct { int fields[6]; ngx_uint_t count; } ac_metadata_t;
typedef struct { ngx_flag_t proxy; } ac_loc_conf_t;

typedef struct {
    ngx_str_t source;
    ngx_str_t prefix;
    ngx_regex_t *regex;
    ngx_uint_t captures;
    ngx_array_t names;
    unsigned exact:1;
} ac_path_t;

typedef struct {
    int value_ref;
    int wrapper_ref;
    int service_ref;
    ngx_uint_t resolve_status;
    ngx_str_t id;
    ngx_int_t priority;
    ngx_array_t paths;
    ngx_array_t hosts;
    ngx_array_t cidrs;
    ngx_uint_t methods;
    int vars_ref;
    int filter_ref;
    ac_plan_t plan;
    ac_upstream_t *upstream;
    ac_expr_t *expression;
    ac_metadata_t *metadata;
    ac_key_auth_conf_t *key_auth;
} ac_route_t;

typedef struct {
    ac_route_t *base;
    ac_route_t *route;
    const void *consumer;
    const void *group;
    int error_ref;
    ngx_flag_t changed;
    ngx_uint_t status;
    ac_metadata_t *metadata;
} ac_consumer_route_t;

typedef struct ac_radix_node_s ac_radix_node_t;
typedef struct ac_generation_s ac_generation_t;

typedef struct {
    ac_route_t *route;
    ac_path_t *path;
    ngx_str_t host;
    ngx_int_t host_rank;
    ngx_uint_t sequence;
} ac_candidate_t;

typedef struct {
    ngx_pool_t *pool;
    lua_State *vm;
    ngx_array_t plugins;
    ngx_array_t routes;
    ngx_array_t consumer_routes;
    ac_radix_node_t *route_index;
    ac_plan_t global;
    int resources_ref;
    int store_ref;
    int local_ref;
    int var_getters_ref;
    int var_meta_ref;
    int consumer_plugins_ref;
    int consumer_indexes_ref;
    int resolver_names_ref;
    int dns_client_ref;
    int hosts_ref;
    int trusted_ref;
    int readers_ref;
    ngx_str_t config_path;
    uint32_t config_crc;
    ngx_uint_t config_version;
    ngx_flag_t native_filters;
    ngx_flag_t native_key_auth;
    ngx_flag_t native_pipeline;
    ngx_flag_t lua_log_bridge;
    ac_key_auth_index_t *key_auth_index;
    ngx_flag_t lazy_rate_json;
    ngx_int_t rate_variable_index;
    ngx_uint_t rate_variable_slot;
    ngx_flag_t log_var_plan;
    ngx_int_t log_var_indexes[5];
    ngx_uint_t log_var_hashes[5];
    ngx_flag_t fused_balancer;
    ngx_flag_t batch_vars;
    ngx_flag_t http_var_plan;
    ngx_flag_t lazy_route_opts;
    ngx_flag_t route_candidates;
    ngx_flag_t request_pool;
    ngx_flag_t request_pool_native;
    ac_table_pool_t *request_pool_state;
    ngx_flag_t matched_pool;
    ac_table_pool_t *matched_pool_state;
    ngx_flag_t balancer_peer_plan;
    ngx_flag_t lazy_meta_matches;
    ngx_flag_t phase_index;
    ngx_flag_t phase_context;
    ngx_flag_t request_header_flags;
    ngx_flag_t header_scratch;
    ngx_flag_t plain_uri;
    ngx_flag_t sdk_cache_replay;
    ngx_flag_t native_var_writes;
    ac_generation_t *generation;
    ac_generation_t *building;
    ac_generation_t *retired;
    unsigned initialized:1;
    unsigned compiled:1;
    unsigned host_router:1;
    unsigned parameter_router:1;
    unsigned proxy_enabled:1;
    unsigned tracing:1;
    unsigned delete_tail_slash:1;
    unsigned servlet_uri:1;
    unsigned encoded_slash:1;
    unsigned server_tokens:1;
    unsigned show_upstream_status:1;
} ac_main_conf_t;

struct ac_generation_s {
    ac_main_conf_t main;
    ngx_array_t refs;
    ngx_uint_t users;
    ac_generation_t *next;
};

typedef struct {
    ngx_http_request_t *request;
    ac_main_conf_t *main;
    ac_generation_t *generation;
    ac_route_t *route;
    ac_metadata_t *metadata;
    ac_path_t *path;
    int table_ref;
    int match_opts_ref;
    int matched_ref;
    ngx_array_t candidates;
    ngx_uint_t candidate_cursor;
    ac_phase_e phase;
    ngx_uint_t stage;
    ngx_uint_t cursor;
    ac_call_t *active;
    unsigned callback_pending:1;
    unsigned pre_pending:1;
    unsigned active_pre:1;
    unsigned phase_entered:1;
    unsigned log_done:1;
    unsigned released:1;
    unsigned routing:1;
    unsigned filter_pending:1;
    ngx_uint_t early_status;
    unsigned consumer_processed:1;
    unsigned consumer_rewrite:1;
    unsigned temporary_route:1;
    ac_upstream_t *upstream;
    ngx_str_t upstream_scheme;
    ngx_str_t upstream_host;
    ngx_str_t upstream_uri;
    ngx_str_t routing_uri;
    ngx_str_t normalized_uri;
    ngx_str_t matched_host;
    ngx_str_t balancer_ip;
    ngx_uint_t balancer_port;
    void *native_consumer;
    unsigned upstream_owned:1;
    ngx_str_t native_variables[64];
    ac_rate_snapshot_t *rate_snapshot;
    ngx_array_t meta_matches;
    uint64_t native_variable_set;
    ngx_uint_t dns_cursor;
    unsigned dns_pending:1;
    unsigned dns_ready:1;
    int span_ref;
    void *native_peer;
    ngx_uint_t balance_attempts;
    unsigned upstream_prepared:1;
    unsigned script_route:1;
    unsigned script_loaded:1;
    unsigned script_dispatched:1;
    unsigned script_pending:1;
    unsigned c_pipeline:1;
    unsigned lua_log_active:1;
    unsigned lua_log_started:1;
    unsigned native_proxied:1;
    unsigned upstream_logged:1;
} ac_request_t;

void ac_key_auth_prepare(lua_State *L, ac_main_conf_t *main);
void ac_key_auth_compile(lua_State *L, ac_main_conf_t *main, ac_call_t *call, ngx_pool_t *pool);
ngx_int_t ac_key_auth_run(ac_request_t *ctx, ac_key_auth_conf_t *conf, ngx_str_t *body);
void ac_pipeline_compile(lua_State *L, ac_main_conf_t *main);
ngx_int_t ac_pipeline_register(ngx_conf_t *cf);
ngx_int_t ac_match_native(ac_request_t *ctx);
ngx_int_t ac_http_prepare_native(ac_request_t *ctx);
ngx_int_t ac_upstream_prepare_native(ac_request_t *ctx);
void ac_upstream_validate_native(lua_State *L, ac_main_conf_t *main, ac_route_t *route);
char *ac_upstream_native_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
void ac_attach_lua_context(lua_State *L, ac_request_t *ctx);
void ac_cleanup(void *data);
void ac_key_auth_publish(lua_State *L, ac_request_t *ctx);
int ac_pipeline_log_start(lua_State *L);

extern ngx_module_t ngx_http_apisix_core_module;
extern const char *ac_phase_names[AC_PHASES];

int ac_table_pool_fetch(lua_State *L, ac_main_conf_t *main, ac_table_pool_t *pool, int nrec);
int ac_abs(lua_State *L, int index);
int ac_ref(lua_State *L, int index);
void ac_push(lua_State *L, int ref);
ngx_str_t ac_string(lua_State *L, ngx_pool_t *pool, int index);
ngx_flag_t ac_bool_field(lua_State *L, int index, const char *name);
void ac_set_string(lua_State *L, int index, const char *name, ngx_str_t value);
void ac_copy(lua_State *L, int index, ngx_uint_t depth);
ac_main_conf_t *ac_main(void);
ac_request_t *ac_request(lua_State *L);
int ac_context(lua_State *L);
int ac_init(lua_State *L);
int ac_init_worker(lua_State *L);
int ac_shutdown(lua_State *L);
int ac_enter(lua_State *L);
int ac_next(lua_State *L);
int ac_complete(lua_State *L);
int ac_leave(lua_State *L);
int ac_phase_start(lua_State *L);
int ac_phase_step(lua_State *L);
void ac_compile_plugins(lua_State *L, ac_main_conf_t *main, int config);
void ac_compile_plan(lua_State *L, ac_main_conf_t *main, ac_plan_t *plan,
    ngx_pool_t *pool, int plugins, ngx_flag_t validate);
void ac_compile_routes(lua_State *L, ac_main_conf_t *main, int config);
void ac_match_start(lua_State *L, ac_request_t *ctx);
int ac_match_next(lua_State *L, ac_request_t *ctx);
void ac_match_complete(lua_State *L, ac_request_t *ctx, ngx_flag_t accepted);
ac_expr_t *ac_expr_compile(lua_State *L, ngx_pool_t *pool, int expression);
int ac_expr_eval(lua_State *L, ac_expr_t *expression, ac_request_t *ctx);
void ac_expr_release(lua_State *L, ac_expr_t *expression);
int ac_encode(lua_State *L);
int ac_render(lua_State *L);
void ac_variable(lua_State *L, ac_request_t *ctx, ngx_str_t name);
void ac_require(lua_State *L, const char *name);
void ac_import(lua_State *L, int target, const char *module, const char *field);
ngx_int_t ac_sdk_register(ngx_conf_t *cf);
ngx_int_t ac_schema_register(ngx_conf_t *cf);
ngx_int_t ac_resources_register(ngx_conf_t *cf);
void ac_resources_prepare(lua_State *L, ac_main_conf_t *main, int config);
void ac_resource(lua_State *L, ac_main_conf_t *main, const char *name);
void ac_resource_get(lua_State *L, ac_main_conf_t *main, const char *name, int key);
void ac_plugin_parent(lua_State *L, int plugins, int parent);
void ac_bind(lua_State *L, lua_CFunction start, int upvalues);
ngx_int_t ac_request_register(ngx_conf_t *cf);
void ac_ngx(lua_State *L, const char *table, const char *field);
ngx_int_t ac_merge_register(ngx_conf_t *cf);
void ac_resolve_route(lua_State *L, ac_main_conf_t *main, ac_route_t *route, int wrapper);
void ac_route_context(lua_State *L, ac_request_t *ctx, ngx_flag_t global);
void ac_route_context_at(lua_State *L, ac_request_t *ctx, ngx_flag_t global, int table);
int ac_var_batch_begin(lua_State *L, ac_request_t *ctx);
ngx_int_t ac_native_var_write_fast(ngx_http_request_t *r, const char *name, ngx_str_t value);
void ac_var_batch_assign(lua_State *L, ac_request_t *ctx, int cache, const char *name,
    ngx_str_t value, ngx_flag_t write_through);
void ac_var_batch_end(lua_State *L, int cache);
void ac_prepare_route_metadata(lua_State *L, ac_main_conf_t *main, ac_route_t *route);
ngx_int_t ac_response_register(ngx_conf_t *cf);
ngx_int_t ac_consumer_register(ngx_conf_t *cf);
ngx_int_t ac_cache_register(ngx_conf_t *cf);
ngx_int_t ac_utils_register(ngx_conf_t *cf);
ngx_int_t ac_instrumentation_register(ngx_conf_t *cf);
ngx_int_t ac_resolver_register(ngx_conf_t *cf);
ngx_int_t ac_background_register(ngx_conf_t *cf);
ngx_int_t ac_platform_register(ngx_conf_t *cf);
ngx_int_t ac_tracing_register(ngx_conf_t *cf);
void ac_phase_begin(lua_State *L, ac_request_t *ctx, ac_phase_e phase);
void ac_phase_finish(lua_State *L, ac_request_t *ctx);
ngx_int_t ac_native_filters_register(ngx_conf_t *cf);
ngx_int_t ac_header_fields(ac_request_t *ctx);
void ac_trace_finish_all(lua_State *L, ac_request_t *ctx, ngx_uint_t code);
void ac_resolver_worker(lua_State *L);
void ac_resolver_utils(lua_State *L, int module);
ngx_int_t ac_upstream_variables(ngx_conf_t *cf);
ngx_int_t ac_request_variables(ngx_conf_t *cf);
ngx_int_t ac_rate_materialize(ac_request_t *ctx, ngx_str_t *result);
void ac_upstream_compile(lua_State *L, ac_main_conf_t *main, ac_route_t *route);
int ac_upstream_prepare(lua_State *L, ac_request_t *ctx);
void ac_upstream_log(lua_State *L, ac_request_t *ctx);
void ac_upstream_api(lua_State *L, int module);
void ac_upstream_cleanup(lua_State *L, ac_request_t *ctx);
ngx_flag_t ac_upstream_enabled(ac_request_t *ctx);
int ac_upstream_next(lua_State *L, ac_request_t *ctx);
void ac_upstream_resolved(lua_State *L, ac_request_t *ctx);
int ac_balance_enter(lua_State *L);
int ac_balance_finish(lua_State *L);
int ac_balance_start(lua_State *L);
void ac_consumer_prepare(lua_State *L, ac_main_conf_t *main);
int ac_consumer_merge(lua_State *L, ac_request_t *ctx);
void ac_sdk_context(lua_State *L, ac_request_t *ctx);
void ac_prefetch_log_vars(lua_State *L, ac_request_t *ctx);
ngx_int_t ac_http_register(ngx_conf_t *cf);
void ac_http_worker(lua_State *L);
ngx_uint_t ac_http_prepare(lua_State *L, ac_request_t *ctx);
void ac_http_match_finish(lua_State *L, ac_request_t *ctx);
void ac_var_assign(lua_State *L, ac_request_t *ctx, const char *name,
    ngx_str_t value, ngx_flag_t write_through);
void ac_native_var_set(lua_State *L, ngx_http_request_t *request,
    const char *name, ngx_str_t value);
ngx_int_t ac_native_var_write(ngx_http_request_t *request, const char *name,
    ngx_str_t value, u_char *error, size_t *error_length);
ngx_int_t ac_native_header_write(ngx_http_request_t *request, ngx_str_t name,
    ngx_str_t value, ngx_flag_t response, ngx_flag_t append, char **error);
void ac_native_header_set(lua_State *L, ngx_http_request_t *request,
    ngx_str_t name, ngx_str_t value, ngx_flag_t response, ngx_flag_t append);
ngx_str_t ac_http_proxy_uri(lua_State *L, ac_request_t *ctx, ngx_str_t uri);
ngx_int_t ac_helpers_register(ngx_conf_t *cf);
int ac_merge_plugin_config(lua_State *L);
int ac_filter_upstream(lua_State *L);
ngx_int_t ac_script_register(ngx_conf_t *cf);
int ac_script_next(lua_State *L, ac_request_t *ctx);
void ac_compile_globals(lua_State *L, ac_main_conf_t *main, int config);
void ac_snapshot_worker(lua_State *L);
void ac_snapshot_acquire(ac_request_t *ctx);
void ac_snapshot_release(lua_State *L, ac_request_t *ctx);
ngx_int_t ac_snapshot_register(ngx_conf_t *cf);
void ac_snapshot_readers(lua_State *L, ac_main_conf_t *main, ngx_flag_t publish);

#endif
