# NextOS teaching SDK. Build tools only; no game data or private base image.
FROM debian@sha256:58ce6f1271ae1c8a2006ff7d3e54e9874d839f573d8009c20154ad0f2fb0a225
ARG DEBIAN_FRONTEND=noninteractive
ADD https://archive.debian.org/debian/dists/buster/Release /opt/debian-buster.Release
ADD https://archive.debian.org/debian-security/dists/buster/updates/Release /opt/debian-security.Release
RUN echo 'f0c7d2d43702c4312f67106aa4addb3ea2ace58f2f761cd6aa8aabce55977dc8  /opt/debian-buster.Release' | sha256sum -c - \
 && echo '82c249d9156ea99429251ac80b4fe40943d2692757db31f7ff9115e70df9c2f2  /opt/debian-security.Release' | sha256sum -c - \
 && dpkg --add-architecture arm64 \
 && printf '%s\n' 'deb [check-valid-until=no] http://archive.debian.org/debian buster main' 'deb [check-valid-until=no] http://archive.debian.org/debian-security buster/updates main' > /etc/apt/sources.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl git make gcc g++ \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross \
    gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
    clang lld python3 python3-pip ninja-build pkg-config qemu-user aapt zlib1g-dev libffi-dev libssl-dev \
    libsdl2-dev:arm64 libegl1-mesa-dev:arm64 libgles2-mesa-dev:arm64 \
 && curl --fail --location https://archive.debian.org/debian/dists/buster/Release -o /opt/debian-buster.current.Release \
 && cmp /opt/debian-buster.Release /opt/debian-buster.current.Release \
 && curl --fail --location https://archive.debian.org/debian-security/dists/buster/updates/Release -o /opt/debian-security.current.Release \
 && cmp /opt/debian-security.Release /opt/debian-security.current.Release \
 && rm -rf /var/lib/apt/lists/*
ADD https://files.pythonhosted.org/packages/51/e0/fb9182958e9c07366f991cf75fdc899a8d8bca6855d47d97728f5fdb15b2/cmake-3.22.6-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl /opt/cmake.whl
RUN echo 'a2357c510e37db7df156c31ab43857b4583e3a2ece8220747e8a007aea50aa45  /opt/cmake.whl' | sha256sum -c - \
 && mv /opt/cmake.whl /opt/cmake-3.22.6-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl \
 && python3 -m zipfile -e /opt/cmake-3.22.6-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl /opt/cmake-distribution \
 && chmod 0755 /opt/cmake-distribution/cmake/data/bin/* \
 && ln -s /opt/cmake-distribution/cmake/data/bin/cmake /usr/local/bin/cmake \
 && ln -s /opt/cmake-distribution/cmake/data/bin/ctest /usr/local/bin/ctest \
 && ln -s /opt/cmake-distribution/cmake/data/bin/cpack /usr/local/bin/cpack \
 && dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\n' > /opt/nextos-sdk-packages.tsv
ENV LC_ALL=C.UTF-8
ADD https://www.kernel.org/pub/software/scm/git/git-2.45.2.tar.xz /opt/git-source.tar.xz
RUN echo '51bfe87eb1c02fed1484051875365eeab229831d30d0cec5d89a14f9e40e9adb  /opt/git-source.tar.xz' | sha256sum -c - \
 && python3 -c "import tarfile; tarfile.open('/opt/git-source.tar.xz').extractall('/opt')" \
 && make -C /opt/git-2.45.2 -j2 prefix=/usr/local NO_GETTEXT=YesPlease NO_TCLTK=YesPlease NO_PERL=YesPlease NO_CURL=YesPlease NO_EXPAT=YesPlease NO_OPENSSL=YesPlease install \
 && git --version \
 && rm -rf /opt/git-2.45.2 /opt/git-source.tar.xz
WORKDIR /src
ADD https://www.python.org/ftp/python/3.11.9/Python-3.11.9.tar.xz /opt/python-source.tar.xz
RUN echo '9b1e896523fc510691126c864406d9360a3d1e986acbda59cda57b5abda45b87  /opt/python-source.tar.xz' | sha256sum -c - \
 && /usr/bin/python3 -c "import tarfile; tarfile.open('/opt/python-source.tar.xz').extractall('/opt')" \
 && cd /opt/Python-3.11.9 \
 && ./configure --prefix=/opt/python311 --without-ensurepip --disable-test-modules \
 && make -j2 \
 && make install \
 && ln -s /opt/python311/bin/python3 /usr/local/bin/python3 \
 && /usr/local/bin/python3 -c 'import ctypes, ssl, zlib; print("Python SDK ready")' \
 && rm -rf /opt/Python-3.11.9 /opt/python-source.tar.xz
CMD ["bash"]
