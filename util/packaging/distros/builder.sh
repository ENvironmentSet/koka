#!/bin/sh

# This script is ran from inside the distro specific docker containers
# It automates the building of koka with stack and then cleans everything except the bundles

DISTRO=""
BUILD_MODE=""

GLIBC_VERSION=""
KOKA_VERSION=""
ARCHITECTURE=""

LOG_PREFIX="[KOKA INTERNAL BUILDER] "

info() {
  echo "$LOG_PREFIX$@"
}

warn() {
  echo "$LOG_PREFIX$@" >&2
}

stop() {
  warn $@
  exit 1
}

get_glibc_version() {
  GLIBC_VERSION=$(ldd --version | head -n 1)
  GLIBC_VERSION=$(echo $GLIBC_VERSION | awk '{print $NF}')
  echo $GLIBC_VERSION
}

get_koka_version() {
  kk_version=""
  if [ "$BUILD_MODE" = "stack" ]; then
    kk_version=$(stack exec koka -- --version --console=raw | grep "Koka ")
  elif [ "$BUILD_MODE" = "cabal" ]; then
    kk_version=$(cabal new-run koka -- --version --console=raw | grep "Koka ")
  fi

  kk_version="${kk_version%%,*}"   # remove everything after the first ",*"
  kk_version="${kk_version#Koka }" # remove "Koka " prefix
  echo $kk_version
}

get_koka_arch() {
  # We need this because of the koka bug, this can be removed once the koka bug is fixed
  export koka_code="\n:l std/os/env\nprintln(\"KK_arch: \" ++ get-cpu-arch())"

  if [ "$BUILD_MODE" = "stack" ]; then
    kk_arch=$(script --return --quiet -c "echo -e \"\$koka_code\" | stack exec koka" /dev/null)
  elif [ "$BUILD_MODE" = "cabal" ]; then
    kk_arch=$(script --return --quiet -c "echo -e \"\$koka_code\" | cabal new-run koka" /dev/null)
  fi
  unset koka_code

  kk_arch=$(echo "$kk_arch" | grep -Pom 1 "(?<=KK_arch: )[a-zA-Z0-9]+")
  echo "$kk_arch"
}

mount_overlay() {
  info "Mounting overlay"
  # Check if /proc/filesystems contains overlayfs and tmpfs
  if ! grep -q overlay /proc/filesystems || ! grep -q tmpfs /proc/filesystems; then
    stop "Your system does not support overlayfs or tmpfs, it needs this to build"
  fi

  mkdir -p /tmp/overlay

  mount -t tmpfs tmpfs /tmp/overlay

  if [ $? -ne 0 ]; then
    stop "Failed to mount tmpfs"
  fi

  mkdir -p /tmp/overlay/coderw /tmp/overlay/codework

  mount -t overlay overlay -o lowerdir=/code,upperdir=/tmp/overlay/coderw,workdir=/tmp/overlay/codework /tmp/overlay/coderw
  if [ $? -ne 0 ]; then
    stop "Failed to mount overlayfs, the container needs the SYS_ADMIN capability"
  fi

  cd /tmp/overlay/coderw

  info "Overlay mounted"
}

build_koka() {
  info "Building koka"

  status=1
  if [ "$BUILD_MODE" = "stack" ]; then
    stack build
    status=$?
  elif [ "$BUILD_MODE" = "cabal" ]; then
    cabal configure --enable-executable-static
    cabal new-build
    status=$?
  fi

  if [ $status -ne 0 ]; then
    stop "Failed to build koka"
  fi

  info "Koka built"
}

bundle_koka() {
  info "Bundling koka"

  postfix="v$KOKA_VERSION-$DISTRO-$ARCHITECTURE"

  status=1
  if [ "$BUILD_MODE" = "stack" ]; then
    script --return --quiet -c "stack exec koka -- -e util/bundle -- --postfix=\"\\\"$postfix\\\"\"" /dev/null
    status=$?
  elif [ "$BUILD_MODE" = "cabal" ]; then
    script --return --quiet -c "cabal new-run koka -- -e util/bundle -- --postfix=\"\\\"$postfix\\\"\"" /dev/null
    status=$?
  fi

  if [ $status -ne 0 ]; then
    stop "Failed to bundle koka"
  fi

  info "Koka bundled"
}

export_build() {
  info "Exporting build"

  cp ./bundle/*.tar.gz /output/

  if [ $? -ne 0 ]; then
    stop "Failed to export bundle"
  fi

  info "Exported build"
}

full_build() {
  info "Starting build"

  mount_overlay

  build_koka

  ARCHITECTURE=$(get_koka_arch)
  KOKA_VERSION=$(get_koka_version)

  bundle_koka

  info "Koka version: $KOKA_VERSION"
  info "Build mode: $BUILD_MODE"
  info "Distro: $DISTRO"
  info "GLIBC Version: $GLIBC_VERSION"
  info "Architecture: $ARCHITECTURE"

  export_build

  info "Build finished"
}

init_parse_param() {
  # First param is the distro, if exists
  if [ -n "$1" ]; then
    DISTRO="$1"
  fi

  # Second param is BUILD_MODE, if exists
  if [ -n "$2" ]; then
    BUILD_MODE="$2"
  fi

  # Default distro is unknown
  if [ -z "$DISTRO" ]; then
    DISTRO="unknown"
  fi

  # Default build mode is stack
  if [ -z "$BUILD_MODE" ]; then
    BUILD_MODE="stack"
  fi

  GLIBC_VERSION=$(get_glibc_version)

  full_build
}

init_parse_param "$@"
