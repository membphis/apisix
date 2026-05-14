# APISIX config for ai-proxy single-CPU streaming benchmark.
# Mounted into the apache/apisix:dev container at run time by run.sh.
# Hand-edit this file (not a copy) and rerun run.sh.

deployment:
  role: traditional
  role_traditional:
    config_provider: etcd
  etcd:
    host:
      - http://127.0.0.1:2379
  admin:
    admin_key:
      - name: admin
        key: edd1c9f034335f136f87ad84b625c8f1
        role: admin

nginx_config:
  worker_processes: 1
  error_log_level: warn

# prometheus must be listed so its lua_shared_dict is generated;
# ai-proxy requires it internally.
plugins:
  - ai-proxy
  - prometheus
