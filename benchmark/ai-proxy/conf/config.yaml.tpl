# APISIX config for ai-proxy single-CPU streaming benchmark.
# Run-time copy of this file is placed at conf/config.yaml by run.sh.
# Hand-edit this file (not the copy) and rerun run.sh.

deployment:
  role: traditional
  role_traditional:
    config_provider: yaml
  admin:
    admin_key:
      - name: admin
        key: edd1c9f034335f136f87ad84b625c8f1
        role: admin

nginx_config:
  worker_processes: 1
  error_log_level: warn
  http:
    access_log: 'off'

plugins:
  - ai-proxy
