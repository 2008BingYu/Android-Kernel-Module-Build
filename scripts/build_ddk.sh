#!/bin/bash
# DDK 模式编译：用 $KDIR（DDK 容器里预置，或本地内核树）编译树外模块
# 用法:
#   build_ddk.sh [kmi...]      # 不填则用 $KERNELS，再不行扫描 $KERNELS_ROOT
#
# 云编译：KSU/DDK 容器里 KDIR 已就绪，workflow 传一个 kmi 调用
# 本地编译：设 KERNELS_ROOT（默认 ./android-kernel），每个版本一个子目录，
#           也可用 KDIR 直接指定单版本内核构建目录
set -euo pipefail

BUILD_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname -- "$BUILD_ROOT")"

if [[ -n "${GITHUB_WORKSPACE:-}" ]]; then
    KERNELS_ROOT="${KERNELS_ROOT:-$GITHUB_WORKSPACE/kernels}"
    DRIVER_SRC="$GITHUB_WORKSPACE/modules"
else
    KERNELS_ROOT="${KERNELS_ROOT:-$REPO_ROOT/android-kernel}"
    DRIVER_SRC="$REPO_ROOT/modules"
fi

DIST_ROOT="${GITHUB_WORKSPACE:+$GITHUB_WORKSPACE/dist}"
DIST_ROOT="${DIST_ROOT:-$REPO_ROOT/dist}"

GREEN='\e[32m'; RED='\e[31m'; YELLOW='\e[33m'; CYAN='\e[36m'; BLUE='\e[34m'; NC='\e[0m'
declare -a BUILD_RESULTS=()

log_info()  { echo -e "  ${GREEN}✔${NC} $*"; }
log_warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
log_error() { echo -e "  ${RED}✘${NC} $*"; }
log_step()  { echo -e "\n${CYAN}[$1]${NC} $2"; }
log_title() { echo -e "\n${BLUE}============================================================${NC}"; }

clean_driver_build() {
    [[ -d "$DRIVER_SRC" ]] || return 0
    find "$DRIVER_SRC" -type f \( \
        -name '*.o' -o -name '*.o.d' -o -name '*.mod' -o -name '*.mod.c' -o \
        -name '*.order' -o -name '*.symvers' -o -name '*.cmd' -o -name '*.usyms' -o \
        -name '*.lds' -o -name '*.mod.o' -o -name '*.lto.o' -o -name '*.ko.lds' \
    \) -delete 2>/dev/null || true
    find "$DRIVER_SRC" -type d -name '.tmp_versions' -prune -exec rm -rf -- {} + 2>/dev/null || true
}

handle_output() {
    local version="$1"

    local source_ko
    source_ko=$(find "$DRIVER_SRC" -maxdepth 1 -name "*.ko" | head -n 1)
    if [[ -z "$source_ko" || ! -f "$source_ko" ]]; then
        log_error "编译失败: 未找到 .ko"
        BUILD_RESULTS+=("$version: 编译失败")
        return 1
    fi

    local mod_name
    mod_name=$(basename "${source_ko%.ko}")
    local git_sha
    git_sha=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)

    local target_dir="$DIST_ROOT/$version"
    mkdir -p "$target_dir"
    local base_name="${mod_name}-${version}-${git_sha}"
    local target_ko="$target_dir/${base_name}.ko"

    cp -f "$source_ko" "$target_ko"
    rm -f "$source_ko"

    if command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
        aarch64-linux-gnu-strip --strip-debug "$target_ko" || true
    else
        strip --strip-debug "$target_ko" 2>/dev/null || true
    fi

    ( cd "$target_dir" && sha256sum "${base_name}.ko" >"${base_name}.ko.sha256" )

    local vermagic="unknown"
    if command -v modinfo >/dev/null 2>&1; then
        vermagic=$(modinfo -F vermagic "$target_ko" 2>/dev/null || true)
    fi
    [[ -n "$vermagic" ]] || vermagic=$(strings "$target_ko" 2>/dev/null | grep -m1 '^vermagic=' || echo unknown)

    {
        echo "version=$version"
        echo "file=${base_name}.ko"
        echo "git_sha=$git_sha"
        echo "size=$(stat -c%s "$target_ko" 2>/dev/null || echo 0)"
        echo "vermagic=$vermagic"
        echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$target_dir/build-info.txt"

    BUILD_RESULTS+=("$version: 成功")
    log_info "产物: dist/$version/${base_name}.ko"
    log_info "vermagic: $vermagic"
}

build_one() {
    local version="$1"
    local kdir="${2:-}"

    if [[ -z "$kdir" ]]; then
        kdir="$KERNELS_ROOT/$version"
    fi

    log_title
    log_step "$version" "DDK 编译"

    if [[ ! -d "$kdir" ]]; then
        log_error "找不到内核构建目录: $kdir"
        BUILD_RESULTS+=("$version: 缺少内核目录")
        return 1
    fi
    log_info "KDIR=$kdir"

    if [[ ! -f "$kdir/Module.symvers" ]]; then
        log_warn "Module.symvers 不存在，先执行 modules_prepare"
        if ! make -C "$kdir" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
                 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare -j"$(nproc)"; then
            log_error "modules_prepare 失败"
            BUILD_RESULTS+=("$version: modules_prepare 失败")
            return 1
        fi
    fi

    log_step "$version" "编译模块"
    clean_driver_build
    if ! make -C "$kdir" M="$DRIVER_SRC" \
            ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
            modules -j"$(nproc)"; then
        log_error "编译失败"
        BUILD_RESULTS+=("$version: 编译失败")
        clean_driver_build
        return 1
    fi

    handle_output "$version"
    clean_driver_build
}

main() {
    local requested=("$@")

    if [[ ${#requested[@]} -eq 0 && -n "${KERNELS:-}" ]]; then
        read -r -a requested <<<"$KERNELS"
    fi

    # 云端只有一个 KDIR，直接用
    if [[ -n "${KDIR:-}" && -d "${KDIR:-}" ]]; then
        if [[ ${#requested[@]} -eq 0 ]]; then
            log_error "缺少版本参数（例如 build_ddk.sh android14-6.1）"
            exit 2
        fi
        for v in "${requested[@]}"; do
            build_one "$v" "$KDIR" || true
        done
    else
        # 本地：每个版本一个子目录
        if [[ ${#requested[@]} -eq 0 ]]; then
            mapfile -t requested < <(find "$KERNELS_ROOT" -maxdepth 1 -mindepth 1 -type d -printf '%f\n' 2>/dev/null | sort)
        fi
        if [[ ${#requested[@]} -eq 0 ]]; then
            log_error "没有可编译的版本：请传参数、设 KERNELS 或检查 $KERNELS_ROOT"
            exit 2
        fi
        for v in "${requested[@]}"; do
            build_one "$v" "$KERNELS_ROOT/$v" || true
        done
    fi

    log_title
    local failed=0 total=0
    for r in "${BUILD_RESULTS[@]}"; do
        total=$((total + 1))
        [[ "$r" == *成功* ]] || failed=$((failed + 1))
        echo -e "  $r"
    done
    log_title
    log_info "汇总: 共 $total 个，成功 $((total - failed)) 个，失败 $failed 个"
    [[ $failed -eq 0 ]]
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
