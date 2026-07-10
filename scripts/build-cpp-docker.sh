#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
image=noggit-purple:local
output="$repo/out"
container=

cleanup() {
  if [[ -n "$container" ]]; then
    docker rm -f "$container" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

docker build --build-arg "BUILD_JOBS=${BUILD_JOBS:-2}" -t "$image" "$repo"
container=$(docker create "$image")
mkdir -p "$output/lib"
docker cp "$container:/build/bin/." "$output/"

for library in \
  /lib/libstorm.so.9 \
  /lib/x86_64-linux-gnu/liblua5.4.so.0 \
  /lib/x86_64-linux-gnu/libbz2.so.1.0 \
  /lib/x86_64-linux-gnu/libtomcrypt.so.1
do
  docker cp -L "$container:$library" "$output/lib/"
done

echo "Noggit exporté dans $output/noggit"
echo 'Lancement : (cd out && LD_LIBRARY_PATH="$PWD/lib" ./noggit)'
