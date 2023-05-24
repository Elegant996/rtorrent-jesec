ARG BUILD_IMAGE=ubuntu:22.04
ARG ALPINE_IMAGE=alpine:3.18

FROM curlimages/curl:8.00.1 as curl

RUN curl -L -o /tmp/bazel https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64

FROM ${BUILD_IMAGE} as build

COPY --from=curl --chmod=755 /tmp/bazel /usr/local/bin/bazel

ENV RTORRENT_VERSION 0.9.8
ENV RTORRENT_REVISION r17

WORKDIR /root/rtorrent

# Install build dependencies
RUN apt-get update && apt-get install -y \
      build-essential \
      git \
      python-is-python3 \
      python3

# # Checkout rTorrent sources from Github repository
RUN git clone --depth 1 https://github.com/Elegant996/rtorrent .

# Set architecture for packages
RUN sed -i 's/architecture = \"all\"/architecture = \"amd64\"/' BUILD.bazel

# Build rTorrent packages
RUN bazel build rtorrent-deb --features=fully_static_link --verbose_failures

# Copy outputs
RUN mkdir dist
RUN cp -L bazel-bin/rtorrent dist/
RUN cp -L bazel-bin/rtorrent-deb.deb dist/

# Now get the clean image
FROM ${ALPINE_IMAGE} as build-sysroot

WORKDIR /root

# Fetch runtime dependencies
RUN apk --no-cache add \
      binutils \
      ca-certificates \
      curl \
      ncurses-terminfo-base

# Install rTorrent and dependencies to new system root
RUN mkdir -p /root/sysroot/etc/ssl/certs
COPY --from=build /root/rtorrent/dist/rtorrent-deb.deb .
RUN ar -xv rtorrent-deb.deb
RUN tar xvf data.tar.* -C /root/sysroot/

RUN mkdir -p /root/sysroot/download /root/sysroot/session /root/sysroot/watch

FROM ${ALPINE_IMAGE} as rtorrent

RUN apk --no-cache add \
      bash \
      curl \
      grep \
      mktorrent \
      tini \
      tzdata \
      unzip \
    && apk --no-cache upgrade \
       -X https://dl-cdn.alpinelinux.org/alpine/v3.14/main \
       unrar

COPY --from=build-sysroot /root/sysroot /

EXPOSE 5000

STOPSIGNAL SIGHUP

ENV HOME=/download

ENTRYPOINT [ "/sbin/tini", "--" ]
CMD [ "/usr/bin/rtorrent" ]