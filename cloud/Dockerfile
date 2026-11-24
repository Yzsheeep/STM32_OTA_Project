FROM node:24-bookworm-slim
WORKDIR /srv/ota
RUN mkdir /data && chown node:node /data
COPY service.mjs ./
USER node
ENV OTA_BIND=0.0.0.0 OTA_PORT=18080 OTA_DB_PATH=/data/firmware.sqlite OTA_TOKEN_FILE=/run/secrets/admin_token
CMD ["node", "service.mjs"]
