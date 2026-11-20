# Build with the official Arduino UNO Q toolchain. This never contacts a board.
FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad AS toolchain
ARG TARGETARCH
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl bzip2 python3 \
    && rm -rf /var/lib/apt/lists/*
RUN case "$TARGETARCH" in \
      amd64) arduino_arch=64bit; sha=321ee249f44c78053234fec0f36101092bc6021bf24939e15e0cc2449c5ed250 ;; \
      arm64) arduino_arch=ARM64; sha=2ed4e1436bb3be1265966af8eba95dd25e28f3a9f92d0a58d371ed14dbaf550f ;; \
      *) echo "Arduino UNO Q toolchain requires amd64 or arm64" >&2; exit 1 ;; \
    esac \
    && curl -fsSL --retry 3 "https://downloads.arduino.cc/arduino-cli/arduino-cli_1.6.0_Linux_${arduino_arch}.tar.gz" -o /tmp/arduino-cli.tar.gz \
    && echo "$sha  /tmp/arduino-cli.tar.gz" | sha256sum -c \
    && tar -xzf /tmp/arduino-cli.tar.gz -C /usr/local/bin arduino-cli \
    && rm /tmp/arduino-cli.tar.gz \
    && arduino-cli config init \
    && arduino-cli config set network.connection_timeout 180s \
    && arduino-cli core update-index \
    && arduino-cli core install arduino:zephyr@1.1.0

FROM toolchain AS build
COPY firmware/uno_q/ /src/uno_q/
RUN arduino-cli compile --fqbn arduino:zephyr:unoq --warnings all --output-dir /firmware /src/uno_q

FROM scratch AS firmware
COPY --from=build /firmware/ /
