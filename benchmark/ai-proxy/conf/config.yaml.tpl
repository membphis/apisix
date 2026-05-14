# APISIX config for ai-proxy single-CPU streaming benchmark.
# Standalone mode: routes come from conf/apisix.yaml (no etcd, no admin API).
# Mounted read-only into the apache/apisix:dev container by run.sh.

deployment:
  role: data_plane
  role_data_plane:
    config_provider: yaml

nginx_config:
  worker_processes: 1
  error_log_level: warn
  http:
    enable_access_log: false

plugins:
  - ai-proxy
  # prometheus must be listed so its lua_shared_dict is generated;
  # ai-proxy requires it internally.
  - prometheus
