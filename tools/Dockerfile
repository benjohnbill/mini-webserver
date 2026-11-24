FROM ubuntu:22.04
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential curl apache2-utils python3 netcat-openbsd ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
