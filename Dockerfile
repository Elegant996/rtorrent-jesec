FROM ubuntu:22.04 AS build

ADD --chmod=755 https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 /usr/local/bin/bazel

ENV RTORRENT_VERSION=0.9.8
ENV RTORRENT_REVISION=r17

WORKDIR /root/rtorrent

# Install build dependencies
RUN apt-get update && apt-get install -y \
      build-essential \
      git \
      python-is-python3 \
      python3

# Checkout rTorrent sources from current directory
COPY . ./

# # Checkout rTorrent sources from Github repository
# RUN git clone --depth 1 https://github.com/Elegant996/rtorrent .

# Set architecture for packages
RUN sed -i 's/architecture = \"all\"/architecture = \"amd64\"/' BUILD.bazel

# Build rTorrent packages
RUN bazel build rtorrent-deb --features=fully_static_link --verbose_failures

# Copy outputs
RUN mkdir dist
RUN cp -L bazel-bin/rtorrent dist/
RUN cp -L bazel-bin/rtorrent-deb.deb dist/

# Now get the clean image
FROM alpine:3.20 AS build-sysroot

WORKDIR /root

# Fetch runtime dependencies
RUN mkdir -p /sysroot/etc/apk && cp -r /etc/apk/* /sysroot/etc/apk/
RUN apk add --no-cache --initdb -p /sysroot \
      alpine-baselayout \
      busybox \
      ca-certificates \
      mktorrent \
      ncurses-terminfo-base \
      tini \
      tzdata \
      unzip \
    && apk --no-cache upgrade \
       -X https://dl-cdn.alpinelinux.org/alpine/v3.14/main \
       unrar
RUN rm -rf /sysroot/etc/apk /sysroot/lib/apk /sysroot/var/cache

# Install rTorrent to new system root
RUN mkdir -p /sysroot/etc/rtorrent /sysroot/download /sysroot/session /sysroot/watch
COPY --from=build /root/rtorrent/dist/rtorrent /sysroot/usr/local/bin
COPY doc/rtorrent.rc /sysroot/etc/rtorrent

FROM scratch AS rtorrent

COPY --from=build-sysroot /sysroot /

EXPOSE 5000

STOPSIGNAL SIGHUP

ENV HOME=/download

ENTRYPOINT [ "/sbin/tini", "--" ]
CMD [ "/usr/local/bin/rtorrent" ]