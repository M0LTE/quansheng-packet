# Pinned build environment for the UV-K5 packet firmware.
#
# Ubuntu 22.04 ships arm-none-eabi-gcc 10.3.1, the version upstream
# recommends; newer gcc (13, 14) produces larger images on this code base.
# The base image is pinned by digest and the toolchain packages by version,
# so a local build (compile-with-docker.sh) and the release workflow produce
# the same bytes from the same source and version string.
FROM ubuntu@sha256:b8b6ee6aa931ecd9d0d952abc34dc0e5f7c6a30c6bb71b079fe399fde0329c02
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-none-eabi=15:10.3-2021.07-4 \
      libnewlib-arm-none-eabi=3.3.0-1.3 \
      make python3 python3-crcmod git \
 && rm -rf /var/lib/apt/lists/*
