# Routes loaded by APISIX in yaml/standalone mode.
# Mounted read-only at /usr/local/apisix/conf/apisix.yaml by run.sh.

routes:
  -
    id: 1
    uri: /v1/chat/completions
    plugins:
      ai-proxy:
        provider: openai
        auth:
          header:
            Authorization: "Bearer sk-bench"
        override:
          endpoint: "http://127.0.0.1:1981/v1/chat/completions"

#END
