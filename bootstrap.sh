#!/usr/bin/env bash
#
# bootstrap.sh — install the toolchain this repository is pinned to.
#
#   * asdf itself, if it is not already installed (pinned version, checksummed)
#   * the runtimes named in .tool-versions (nodejs, python)
#   * PlatformIO, from requirements.txt, into a project-local .venv/
#   * the host client's npm dependencies, from host/package-lock.json
#
# Idempotent — running it twice does nothing the second time. Non-interactive,
# so it works as a CI step; on GitHub Actions it also appends the shim and venv
# directories to $GITHUB_PATH so later steps find `pio`, `node` and `npm`.
#
# Linux and macOS. Usage:
#
#   ./bootstrap.sh              install everything
#   ./bootstrap.sh --check      verify an existing install, change nothing
#   ./bootstrap.sh --no-asdf    use the node/python already on PATH instead
#                               (for CI that prefers setup-node/setup-python)
#
set -euo pipefail

ASDF_VERSION="${ASDF_VERSION:-0.20.0}"

# sha256 of the asdf release tarballs for ASDF_VERSION. Pinned rather than
# fetched: this script downloads a binary and runs it, so the digest is the only
# thing standing between a compromised release and every machine that bootstraps.
# Bumping ASDF_VERSION means recomputing all four.
asdf_sha256() {
    case "$1" in
    linux-amd64) echo 9c25e1af7cc4c9d59ff3736eba14fd000480c32929258f80d8c5a8b290ebee14 ;;
    linux-arm64) echo bbc1889886a9826ce3f57f56e4bae575767a4af3d35d649d62116ee14334e59a ;;
    darwin-amd64) echo 8217f33fd165131546aa034f3dabd1a6978cced71c271b4079b4f880965554c1 ;;
    darwin-arm64) echo cc94a8cb12bd9692cd760ca184291552fd7206cf8306e51e4de45db94eea3cb5 ;;
    *) echo "" ;;
    esac
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$REPO_ROOT/.venv"
TOOL_VERSIONS="$REPO_ROOT/.tool-versions"
export ASDF_DATA_DIR="${ASDF_DATA_DIR:-$HOME/.asdf}"

USE_ASDF=1
CHECK_ONLY=0
WANT_FIRMWARE=1
TMPWORK=""

# ---------------------------------------------------------------- output ----

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_OFF=$'\033[0m'
else
    C_BOLD="" C_DIM="" C_RED="" C_GREEN="" C_YELLOW="" C_OFF=""
fi

step() { printf '\n%s==> %s%s\n' "$C_BOLD" "$*" "$C_OFF"; }
info() { printf '    %s\n' "$*"; }
skip() { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_OFF"; }
ok() { printf '    %s✓%s %s\n' "$C_GREEN" "$C_OFF" "$*"; }
bad() { printf '    %s✗%s %s\n' "$C_RED" "$C_OFF" "$*"; }
warn() { printf '%swarning:%s %s\n' "$C_YELLOW" "$C_OFF" "$*" >&2; }
die() {
    printf '%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2
    exit 1
}

# Note the explicit `return 0`: bash takes the script's exit status from the
# last command an EXIT trap ran, so a falsy test here would mask a clean exit.
cleanup() {
    [ -n "$TMPWORK" ] && rm -rf "$TMPWORK"
    return 0
}
trap cleanup EXIT

usage() {
    cat <<'EOF'
bootstrap.sh — install the toolchain this repository is pinned to.

  * asdf itself, if it is not already installed (pinned version, checksummed)
  * the runtimes named in .tool-versions (nodejs, python)
  * PlatformIO, from requirements.txt, into a project-local .venv/
  * the RP2040 platform and toolchain PlatformIO needs to build the firmware
  * the host client's npm dependencies, from host/package-lock.json

Idempotent, non-interactive, Linux and macOS.

Usage:
  ./bootstrap.sh                install everything
  ./bootstrap.sh --check        verify an existing install, change nothing
  ./bootstrap.sh --no-asdf      use the node/python already on PATH instead
                                (for CI that prefers setup-node/setup-python)
  ./bootstrap.sh --no-firmware  skip the RP2040 toolchain download
  ./bootstrap.sh --help         this message

Environment:
  ASDF_VERSION    version of asdf to install if absent (default 0.20.0)
  ASDF_DATA_DIR   where asdf keeps its installs (default ~/.asdf)
EOF
}

# ------------------------------------------------------------ platform ------

detect_platform() {
    case "$(uname -s)" in
    Linux) OS=linux ;;
    Darwin) OS=darwin ;;
    *) die "unsupported OS $(uname -s) — this script handles Linux and macOS" ;;
    esac
    case "$(uname -m)" in
    x86_64 | amd64) ARCH=amd64 ;;
    arm64 | aarch64) ARCH=arm64 ;;
    *) die "unsupported architecture $(uname -m)" ;;
    esac
}

have() { command -v "$1" >/dev/null 2>&1; }

sha256_of() {
    if have sha256sum; then
        sha256sum "$1" | cut -d' ' -f1
    elif have shasum; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "need sha256sum or shasum to verify downloads"
    fi
}

# The version of $1 pinned in .tool-versions, or empty.
tool_version() {
    awk -v tool="$1" '$1 == tool { print $2; exit }' "$TOOL_VERSIONS"
}

# --------------------------------------------------------- system deps ------

# Package names for a header we could not find, per package manager.
pkg_for() {
    case "$PKG_MGR:$1" in
    apt:zlib.h) echo zlib1g-dev ;;
    apt:openssl/ssl.h) echo libssl-dev ;;
    apt:ffi.h) echo libffi-dev ;;
    apt:bzlib.h) echo libbz2-dev ;;
    apt:readline/readline.h) echo libreadline-dev ;;
    apt:sqlite3.h) echo libsqlite3-dev ;;
    apt:lzma.h) echo liblzma-dev ;;
    apt:alsa/asoundlib.h) echo libasound2-dev ;;
    dnf:zlib.h) echo zlib-devel ;;
    dnf:openssl/ssl.h) echo openssl-devel ;;
    dnf:ffi.h) echo libffi-devel ;;
    dnf:bzlib.h) echo bzip2-devel ;;
    dnf:readline/readline.h) echo readline-devel ;;
    dnf:sqlite3.h) echo sqlite-devel ;;
    dnf:lzma.h) echo xz-devel ;;
    dnf:alsa/asoundlib.h) echo alsa-lib-devel ;;
    *) echo "" ;;
    esac
}

detect_pkg_mgr() {
    if have apt-get; then
        PKG_MGR=apt
        PKG_INSTALL="sudo apt-get install -y"
    elif have dnf; then
        PKG_MGR=dnf
        PKG_INSTALL="sudo dnf install -y"
    elif have yum; then
        PKG_MGR=dnf
        PKG_INSTALL="sudo yum install -y"
    else
        PKG_MGR=unknown
        PKG_INSTALL=""
    fi
}

have_header() {
    printf '#include <%s>\n' "$1" | "${CC:-cc}" -E -x c - >/dev/null 2>&1
}

# CPython is built from source by asdf, so its build dependencies have to be
# present before `asdf install` rather than after it fails 200 lines in.
check_build_deps() {
    if [ "$OS" = darwin ]; then
        xcode-select -p >/dev/null 2>&1 ||
            die "the Xcode command line tools are missing — run: xcode-select --install"
        if ! have brew; then
            warn "Homebrew was not found; building CPython needs openssl, and python-build looks for Homebrew's."
            warn "If the Python install fails, install Homebrew and: brew install openssl@3 readline sqlite3 xz"
        fi
        return
    fi

    if ! have "${CC:-cc}"; then
        have gcc ||
            die "no C compiler found — CPython is built from source. Install build-essential (or gcc/make)."
        CC=gcc # so the header probe below does not report everything as missing
    fi

    local required="zlib.h openssl/ssl.h ffi.h"
    local optional="bzlib.h readline/readline.h sqlite3.h lzma.h"
    local missing_required="" missing_optional="" pkgs="" h p

    for h in $required; do
        have_header "$h" || missing_required="$missing_required $h"
    done
    for h in $optional; do
        have_header "$h" || missing_optional="$missing_optional $h"
    done

    [ -n "$missing_required$missing_optional" ] || return 0

    detect_pkg_mgr
    for h in $missing_required $missing_optional; do
        p="$(pkg_for "$h")"
        [ -n "$p" ] && pkgs="$pkgs $p"
    done

    if [ -n "$missing_required" ]; then
        printf '%serror:%s CPython cannot be built here — missing headers:%s\n' \
            "$C_RED" "$C_OFF" "$missing_required" >&2
        [ -n "$pkgs" ] && printf '  install them with: %s%s\n' "$PKG_INSTALL" "$pkgs" >&2
        printf '  or re-run with --no-asdf to use the python already on PATH.\n' >&2
        exit 1
    fi

    warn "optional CPython modules will be missing:$missing_optional"
    [ -n "$pkgs" ] && warn "install with: $PKG_INSTALL$pkgs"
    return 0
}

# ---------------------------------------------------------------- asdf ------

install_asdf() {
    local platform="$OS-$ARCH" expected actual url
    expected="$(asdf_sha256 "$platform")"
    [ -n "$expected" ] ||
        die "no pinned asdf checksum for $platform — install asdf yourself, or re-run with --no-asdf"

    url="https://github.com/asdf-vm/asdf/releases/download/v${ASDF_VERSION}/asdf-v${ASDF_VERSION}-${platform}.tar.gz"
    info "downloading asdf $ASDF_VERSION for $platform"

    TMPWORK="$(mktemp -d)"
    curl -fsSL --retry 3 --retry-delay 2 -o "$TMPWORK/asdf.tar.gz" "$url" ||
        die "could not download $url"

    actual="$(sha256_of "$TMPWORK/asdf.tar.gz")"
    [ "$actual" = "$expected" ] ||
        die "asdf checksum mismatch: expected $expected, got $actual"

    tar -xzf "$TMPWORK/asdf.tar.gz" -C "$TMPWORK"
    mkdir -p "$ASDF_DATA_DIR/bin"
    mv "$TMPWORK/asdf" "$ASDF_DATA_DIR/bin/asdf"
    chmod +x "$ASDF_DATA_DIR/bin/asdf"
    ok "asdf $ASDF_VERSION installed to $ASDF_DATA_DIR/bin/asdf"
    ASDF_WAS_INSTALLED=1
}

ensure_asdf() {
    PATH="$ASDF_DATA_DIR/bin:$ASDF_DATA_DIR/shims:$PATH"
    export PATH
    ASDF_WAS_INSTALLED=0

    if have asdf; then
        skip "asdf already present: $(command -v asdf) ($(asdf version 2>/dev/null | head -1))"
    else
        install_asdf
    fi
}

ensure_plugin() {
    if asdf plugin list 2>/dev/null | grep -q "^$1\$"; then
        skip "plugin $1 already added"
    else
        info "adding plugin $1"
        asdf plugin add "$1"
    fi
}

# Where asdf put $1 $2, or empty if it is not installed. Never fails: callers
# assign it, and under `set -e` a non-zero command substitution would abort the
# script with nothing said.
asdf_where() {
    local dir
    dir="$(asdf where "$1" "$2" 2>/dev/null)" || return 0
    [ -d "$dir" ] && printf '%s' "$dir"
    return 0
}

install_runtimes() {
    ensure_plugin nodejs
    ensure_plugin python

    if [ -z "$(asdf_where python "$PYTHON_VERSION")" ]; then
        check_build_deps
        info "building CPython $PYTHON_VERSION — this takes a few minutes"
    fi

    # `asdf install` with no arguments installs everything .tool-versions names,
    # and is a no-op for versions already present.
    (cd "$REPO_ROOT" && asdf install)
    asdf reshim >/dev/null 2>&1 || true

    NODE_DIR="$(asdf_where nodejs "$NODE_VERSION")"
    PYTHON_DIR="$(asdf_where python "$PYTHON_VERSION")"
    [ -n "$NODE_DIR" ] || die "asdf did not install nodejs $NODE_VERSION"
    [ -n "$PYTHON_DIR" ] || die "asdf did not install python $PYTHON_VERSION"

    NODE_BIN="$NODE_DIR/bin/node"
    NPM_BIN="$NODE_DIR/bin/npm"
    PYTHON_BIN="$PYTHON_DIR/bin/python3"
    ok "node $("$NODE_BIN" --version | sed 's/^v//'), python $("$PYTHON_BIN" -c 'import platform; print(platform.python_version())')"
}

# --no-asdf: take what is on PATH, but refuse a runtime that is not the pinned
# major version, because that is the difference CI would otherwise hide.
use_path_runtimes() {
    have node || die "node is not on PATH (and --no-asdf was given)"
    have npm || die "npm is not on PATH (and --no-asdf was given)"
    have python3 || die "python3 is not on PATH (and --no-asdf was given)"

    NODE_BIN="$(command -v node)"
    NPM_BIN="$(command -v npm)"
    PYTHON_BIN="$(command -v python3)"

    local have_node have_python
    have_node="$("$NODE_BIN" --version | sed 's/^v//')"
    have_python="$("$PYTHON_BIN" -c 'import platform; print(platform.python_version())')"

    [ "${have_node%%.*}" = "${NODE_VERSION%%.*}" ] ||
        die "node ${have_node} is on PATH but .tool-versions pins ${NODE_VERSION}"
    [ "${have_python%.*}" = "${PYTHON_VERSION%.*}" ] ||
        die "python ${have_python} is on PATH but .tool-versions pins ${PYTHON_VERSION}"

    [ "$have_node" = "$NODE_VERSION" ] ||
        warn "node $have_node on PATH, .tool-versions pins $NODE_VERSION"
    [ "$have_python" = "$PYTHON_VERSION" ] ||
        warn "python $have_python on PATH, .tool-versions pins $PYTHON_VERSION"

    ok "node $have_node, python $have_python (from PATH)"
}

# ------------------------------------------------------------ the venv ------

setup_venv() {
    local existing=""
    if [ -x "$VENV/bin/python" ]; then
        existing="$("$VENV/bin/python" -c 'import platform; print(platform.python_version())' 2>/dev/null || echo broken)"
        if [ "$existing" != "$("$PYTHON_BIN" -c 'import platform; print(platform.python_version())')" ]; then
            info "recreating .venv (built against python $existing)"
            rm -rf "$VENV"
        fi
    fi

    if [ ! -x "$VENV/bin/python" ]; then
        info "creating .venv"
        "$PYTHON_BIN" -m venv "$VENV"
        "$VENV/bin/python" -m pip install --quiet --disable-pip-version-check --upgrade pip
    fi

    info "installing $(grep -v '^#' "$REPO_ROOT/requirements.txt" | tr -d '[:space:]')"
    "$VENV/bin/python" -m pip install --quiet --disable-pip-version-check -r "$REPO_ROOT/requirements.txt"
    ok "$("$VENV/bin/pio" --version)"
}

# The RP2040 core and its gcc are a few hundred megabytes, so this is the one
# step worth skipping when only host/ is being worked on. Cache ~/.platformio in
# CI and it becomes as quick as it is on a second local run.
setup_platformio_packages() {
    if [ "$WANT_FIRMWARE" = 0 ]; then
        skip "skipping the RP2040 toolchain (--no-firmware)"
        return
    fi
    info "resolving platform and library dependencies (first run downloads the RP2040 toolchain)"
    (cd "$REPO_ROOT" && "$VENV/bin/pio" pkg install -e pico -e native)
    ok "PlatformIO packages installed"
}

# --------------------------------------------------------- host client ------

setup_host() {
    # @julusian/midi ships prebuilt binaries for the pinned node; only a build
    # from source would need ALSA headers, so this is a warning, not a wall.
    if [ "$OS" = linux ] && ! have_header alsa/asoundlib.h; then
        detect_pkg_mgr
        [ "$PKG_MGR" = unknown ] ||
            warn "alsa/asoundlib.h not found; if @julusian/midi has to compile, run: $PKG_INSTALL $(pkg_for alsa/asoundlib.h)"
    fi

    if [ -f "$REPO_ROOT/host/package-lock.json" ]; then
        info "npm ci in host/"
        (cd "$REPO_ROOT/host" && PATH="$(dirname "$NODE_BIN"):$PATH" "$NPM_BIN" ci --no-audit --no-fund)
    else
        info "npm install in host/"
        (cd "$REPO_ROOT/host" && PATH="$(dirname "$NODE_BIN"):$PATH" "$NPM_BIN" install --no-audit --no-fund)
    fi
    ok "host dependencies installed"
}

# --------------------------------------------------------------- check ------

run_check() {
    local failures=0 v

    if [ "$USE_ASDF" = 1 ]; then
        PATH="$ASDF_DATA_DIR/bin:$ASDF_DATA_DIR/shims:$PATH"
        export PATH
        if have asdf; then
            ok "asdf $(asdf version 2>/dev/null | head -1)"
        else
            bad "asdf not installed"
            failures=$((failures + 1))
        fi
        NODE_BIN="$(asdf_where nodejs "$NODE_VERSION")/bin/node"
        PYTHON_BIN="$(asdf_where python "$PYTHON_VERSION")/bin/python3"
    else
        NODE_BIN="$(command -v node || true)"
        PYTHON_BIN="$(command -v python3 || true)"
    fi

    if [ -x "$NODE_BIN" ] && v="$("$NODE_BIN" --version | sed 's/^v//')" && [ "$v" = "$NODE_VERSION" ]; then
        ok "node $v"
    else
        bad "node $NODE_VERSION not available"
        failures=$((failures + 1))
    fi

    if [ -x "$PYTHON_BIN" ] && v="$("$PYTHON_BIN" -c 'import platform; print(platform.python_version())')" && [ "$v" = "$PYTHON_VERSION" ]; then
        ok "python $v"
    else
        bad "python $PYTHON_VERSION not available"
        failures=$((failures + 1))
    fi

    if [ -x "$VENV/bin/pio" ]; then
        ok "$("$VENV/bin/pio" --version)"
    else
        bad "PlatformIO not installed in .venv"
        failures=$((failures + 1))
    fi

    if [ -d "$REPO_ROOT/host/node_modules" ]; then
        ok "host/node_modules present"
    else
        bad "host dependencies not installed"
        failures=$((failures + 1))
    fi

    if [ "$failures" -gt 0 ]; then
        printf '\n%d check(s) failed — run %s./bootstrap.sh%s\n' "$failures" "$C_BOLD" "$C_OFF" >&2
        exit 1
    fi
    printf '\nEverything is in place.\n'
}

# --------------------------------------------------------------- main -------

while [ $# -gt 0 ]; do
    case "$1" in
    --no-asdf) USE_ASDF=0 ;;
    --no-firmware) WANT_FIRMWARE=0 ;;
    --check) CHECK_ONLY=1 ;;
    -h | --help)
        usage
        exit 0
        ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
    shift
done

detect_platform
have curl || die "curl is required"
have tar || die "tar is required"
# asdf plugins are git clones, and so is the pinned RP2040 platform.
have git || die "git is required"
[ -f "$TOOL_VERSIONS" ] || die "no .tool-versions beside this script"

NODE_VERSION="$(tool_version nodejs)"
PYTHON_VERSION="$(tool_version python)"
[ -n "$NODE_VERSION" ] || die ".tool-versions does not pin nodejs"
[ -n "$PYTHON_VERSION" ] || die ".tool-versions does not pin python"

if [ "$CHECK_ONLY" = 1 ]; then
    step "Checking the toolchain ($OS-$ARCH)"
    run_check
    exit 0
fi

step "Runtimes ($OS-$ARCH)"
if [ "$USE_ASDF" = 1 ]; then
    ensure_asdf
    install_runtimes
else
    use_path_runtimes
fi

step "PlatformIO"
setup_venv
setup_platformio_packages

step "Host client"
setup_host

# Later GitHub Actions steps get `pio`, `node` and `npm` without a shell dance.
# Both asdf directories are needed, not just the shims: a shim is a stub that
# re-execs `asdf`, so a shims-only PATH gives "exec: asdf: not found".
if [ -n "${GITHUB_PATH:-}" ]; then
    [ "$USE_ASDF" = 1 ] && printf '%s\n%s\n' "$ASDF_DATA_DIR/bin" "$ASDF_DATA_DIR/shims" >>"$GITHUB_PATH"
    printf '%s\n' "$VENV/bin" >>"$GITHUB_PATH"
fi

step "Done"
info "pio:  $VENV/bin/pio"
info "node: $NODE_BIN"
cat <<EOF

    Put them on your PATH for this shell:

        export PATH="$VENV/bin:\$PATH"$([ "$USE_ASDF" = 1 ] && printf '\n        export PATH="%s/bin:%s/shims:$PATH"' "$ASDF_DATA_DIR" "$ASDF_DATA_DIR")

    Then:

        pio run -e pico          # build the firmware
        pio test -e native       # engine tests, no hardware
        cd host && npm test      # host tests, no hardware
EOF

if [ "${ASDF_WAS_INSTALLED:-0}" = 1 ]; then
    cat <<EOF
    To make asdf permanent, add to your shell rc:

        export PATH="$ASDF_DATA_DIR/bin:$ASDF_DATA_DIR/shims:\$PATH"
EOF
fi
