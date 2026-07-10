FROM ubuntu:24.04

ARG BUILD_JOBS=2
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libbz2-dev \
        libgl1-mesa-dev \
        libglu1-mesa-dev \
        liblua5.4-dev \
        libqt5opengl5-dev \
        libqt5svg5-dev \
        libqt5x11extras5-dev \
        libstorm-dev \
        qtbase5-dev \
        qtbase5-private-dev \
        qtdeclarative5-dev \
        qtmultimedia5-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel "${BUILD_JOBS}" \
    && ctest --test-dir /build --output-on-failure

RUN chmod a+rwx /build/bin

WORKDIR /build/bin
CMD ["/build/bin/noggit"]
