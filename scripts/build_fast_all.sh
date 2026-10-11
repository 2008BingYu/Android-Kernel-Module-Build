#!/bin/bash

set -euo pipefail

BUILD_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

KERNELS_ROOT="$GITHUB_WORKSPACE/kernels"
DRIVER_SRC="$GITHUB_WORKSPACE/modules"

GREEN='\e[32m'; RED='\e[31m'; YELLOW='\e[33m'
BLUE='\e[34m'; CYAN='\e[36m'; NC='\e[0m'

declare -a BUILD_RESULTS=()

log_info()  { echo -e "  ${GREEN}✔${NC} $*"; }
log_warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
log_error() { echo -e "  ${RED}✘${NC} $*"; }
log_step()  { echo -e "\n${CYAN}[$1]${NC} $2"; }
log_title() { echo -e "\n${BLUE}============================================================${NC}"; }

find_clang_for_kernel() {
    local kernel_dir="$1"
    local declared_ver="" declared_bin=""

    for cfg in \
        "$kernel_dir/common/build.config.common" \
        "$kernel_dir/common/build.config.gki.aarch64" \
        "$kernel_dir/build.config.common" \
        "$kernel_dir/build.config.gki.aarch64"; do
        [[ -f "$cfg" ]] || continue
        local cfg_vars
        cfg_vars=$(
            cd "$kernel_dir" 2>/dev/null || cd /
            env -i PATH="$PATH" ROOT_DIR="$kernel_dir" KERNEL_DIR="common" \
                bash -c "
                    set +u +e
                    source '$cfg' >/dev/null 2>&1 || true
                    echo \"CLANG_VERSION=\${CLANG_VERSION:-}\"
                    echo \"CLANG_PREBUILT_BIN=\${CLANG_PREBUILT_BIN:-}\"
                "
        )
        local v b
        v=$(echo "$cfg_vars" | grep '^CLANG_VERSION=' | cut -d= -f2-)
        b=$(echo "$cfg_vars" | grep '^CLANG_PREBUILT_BIN=' | cut -d= -f2-)
        [[ -n "$v" ]] && declared_ver="$v"
        [[ -n "$b" ]] && declared_bin="$b"
        [[ -n "$declared_ver" || -n "$declared_bin" ]] && break
    done

    if [[ -z "$declared_ver" && -z "$declared_bin" ]]; then
        for scl in "$kernel_dir/bazel/constants.scl" "$kernel_dir/common/bazel/constants.scl"; do
            [[ -f "$scl" ]] || continue
            local scl_ver
            scl_ver=$(grep -E '^[[:space:]]*CLANG_VERSION[[:space:]]*=' "$scl" 2>/dev/null \
                      | head -n1 | cut -d= -f2- | tr -d '"' | tr -d "'" | tr -d ' ')
            [[ -n "$scl_ver" ]] && { declared_ver="$scl_ver"; break; }
        done
    fi

    if [[ -n "$declared_bin" ]]; then
        declared_bin="${declared_bin%/bin}"
        local clang_path
        if [[ "$declared_bin" == /* ]]; then clang_path="$declared_bin"
        else clang_path="$kernel_dir/$declared_bin"; fi
        [[ -x "$clang_path/bin/clang" ]] && { echo "$clang_path"; return 0; }
    fi

    for clang_base in \
        "$kernel_dir/prebuilts-master/clang/host/linux-x86" \
        "$kernel_dir/prebuilts/clang/host/linux-x86"; do
        [[ -d "$clang_base" ]] || continue
        if [[ -n "$declared_ver" && -x "$clang_base/clang-$declared_ver/bin/clang" ]]; then
            echo "$clang_base/clang-$declared_ver"; return 0
        fi
        if [[ -n "$declared_ver" && -x "$clang_base/$declared_ver/bin/clang" ]]; then
            echo "$clang_base/$declared_ver"; return 0
        fi
    done

    for clang_base in \
        "$kernel_dir/prebuilts-master/clang/host/linux-x86" \
        "$kernel_dir/prebuilts/clang/host/linux-x86"; do
        [[ -d "$clang_base" ]] || continue
        local c
        c=$(ls -d "$clang_base"/clang-r* 2>/dev/null | head -1)
        [[ -n "$c" && -x "$c/bin/clang" ]] && { echo "$c"; return 0; }
    done

    return 1
}

fix_empty_ext_modversions() {
    local found=0
    for mod_c in "$DRIVER_SRC"/*.mod.c; do
        [[ -f "$mod_c" ]] || continue
        if grep -q '__section("__version_ext_names")' "$mod_c" && \
           grep -q '^[[:space:]]*;[[:space:]]*$' "$mod_c"; then
            perl -0pi -e 's/(__used __section\("__version_ext_names"\) =\n);/$1"";/' "$mod_c"
            found=1
        fi
    done
    return $((1 - found))
}

clean_driver_build() {
    [[ -d "$DRIVER_SRC" ]] || return 0
    find "$DRIVER_SRC" -type f \( \
        -name '*.o' -o -name '*.o.d' -o -name '*.mod' -o -name '*.mod.c' -o \
        -name '*.order' -o -name '*.symvers' -o -name '*.cmd' -o -name '*.usyms' \
    \) -delete 2>/dev/null || true
    find "$DRIVER_SRC" -type d -name '.tmp_versions' -prune -exec rm -rf -- {} + 2>/dev/null || true
}

cleanup_on_exit() { local s=$?; trap - EXIT; clean_driver_build || true; exit "$s"; }

handle_output() {
    local version="$1"
    local source_ko
    source_ko=$(find "$DRIVER_SRC" -maxdepth 1 -name "*.ko" | head -n 1)
    if [[ -z "$source_ko" || ! -f "$source_ko" ]]; then
        log_error "编译失败: 未找到 .ko"
        BUILD_RESULTS+=("$version: 编译失败")
        return 1
    fi
    local name; name=$(basename "$source_ko")
    local target_dir="$DRIVER_SRC/build/$version"
    mkdir -p "$target_dir"
    cp "$source_ko" "$target_dir/$name"
    rm -f "$source_ko"
    BUILD_RESULTS+=("$version: 成功")
    log_info "产物: build/$version/$name"
}

compile_module() {
    local version="$1" kernel_dir="$2" out_dir="$3"
    local full_path="$4" host_cflags="$5" host_ldflags="$6"

    local symvers_file="$out_dir/Module.symvers"
    local symvers_backup=""
    local modpost_warn_param="KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n"

    if [[ -f "$symvers_file" ]]; then
        symvers_backup="$symvers_file.no_crc_bak.$$"
        mv "$symvers_file" "$symvers_backup"
    fi

    log_step "$version" "编译驱动模块"
    set +e
    env PATH="$full_path" HOSTCFLAGS="$host_cflags" HOSTLDFLAGS="$host_ldflags" \
        make -C "$kernel_dir/common" O="$out_dir" M="$DRIVER_SRC" \
            ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
            HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
            CONFIG_DEBUG_INFO_BTF_MODULES= \
            $modpost_warn_param modules -j"$(nproc)"
    local st=$?

    if [[ $st -ne 0 ]] && fix_empty_ext_modversions; then
        log_warn "检测到空 __version_ext_names，修补后重试"
        env PATH="$full_path" HOSTCFLAGS="$host_cflags" HOSTLDFLAGS="$host_ldflags" \
            make -C "$kernel_dir/common" O="$out_dir" M="$DRIVER_SRC" \
                ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
                HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
                CONFIG_DEBUG_INFO_BTF_MODULES= \
                $modpost_warn_param modules -j"$(nproc)"
        st=$?
    fi
    set -e

    [[ -n "$symvers_backup" ]] && mv "$symvers_backup" "$symvers_file"
    return $st
}

build_legacy() {
    local version="$1"
    local kernel_dir="$KERNELS_ROOT/$version"
    local out_dir="$kernel_dir/out/$version/common/common"

    log_title
    log_step "$version" "编译中 (Legacy 快速模式)"
    [[ -d "$kernel_dir" ]] || { log_error "目录不存在"; BUILD_RESULTS+=("$version: 目录不存在"); return; }
    cd "$kernel_dir" || return
    clean_driver_build

    local clang_path
    clang_path=$(find_clang_for_kernel "$kernel_dir") \
        || { log_error "找不到 Clang"; BUILD_RESULTS+=("$version: 无 Clang"); return; }
    log_info "Clang: $clang_path"

    local build_tools="$kernel_dir/build/build-tools/path/linux-x86"
    local full_path="$clang_path/bin:$build_tools:$PATH"
    local host_cflags="-I$kernel_dir/prebuilts/kernel-build-tools/linux-x86/include -I/usr/include"
    local host_ldflags="-L$kernel_dir/prebuilts/kernel-build-tools/linux-x86/lib64 -L/usr/lib/x86_64-linux-gnu -fuse-ld=lld --rtlib=compiler-rt"

    if [[ ! -f "$out_dir/.config" ]]; then
        log_warn "未找到内核配置，执行快速准备"
        mkdir -p "$out_dir"

        log_step "$version" "生成 gki_defconfig"
        env PATH="$full_path" HOSTCFLAGS="$host_cflags" HOSTLDFLAGS="$host_ldflags" \
            make -C "$kernel_dir/common" O="$out_dir" \
                ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
                CONFIG_DEBUG_INFO_BTF_MODULES= gki_defconfig >/dev/null 2>&1 \
            || { log_error "gki_defconfig 失败"; BUILD_RESULTS+=("$version: 配置失败"); return; }

        log_step "$version" "准备模块构建环境 (modules_prepare)"
        env PATH="$full_path" HOSTCFLAGS="$host_cflags" HOSTLDFLAGS="$host_ldflags" \
            make -C "$kernel_dir/common" O="$out_dir" \
                ARCH=arm64 LLVM=1 LLVM_IAS=1 CONFIG_DEBUG_INFO_BTF_MODULES= \
                CROSS_COMPILE=aarch64-linux-gnu- \
                HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
                modules_prepare -j"$(nproc)" >/dev/null 2>&1 \
            || { log_error "modules_prepare 失败"; BUILD_RESULTS+=("$version: 准备失败"); return; }
        log_info "快速准备完成 (已跳过全量编译)"
    else
        log_info "复用已有配置"
    fi

    if compile_module "$version" "$kernel_dir" "$out_dir" \
                      "$full_path" "$host_cflags" "$host_ldflags"; then
        handle_output "$version"
    else
        log_error "编译失败"
        BUILD_RESULTS+=("$version: 编译失败")
    fi
    clean_driver_build
}

build_bazel() {
    local version="$1"
    local kernel_dir="$KERNELS_ROOT/$version"
    local prep_dir="$kernel_dir/out/manual_prepare"
    local tar_rel="bazel-bin/common/kernel_aarch64_modules_prepare/modules_prepare_outdir.tar.gz"

    log_title
    log_step "$version" "编译中 (Bazel 快速模式)"
    [[ -d "$kernel_dir" ]] || { log_error "目录不存在"; BUILD_RESULTS+=("$version: 目录不存在"); return; }
    cd "$kernel_dir" || return
    clean_driver_build

    local clang_path
    clang_path=$(find_clang_for_kernel "$kernel_dir") \
        || { log_error "找不到 Clang"; BUILD_RESULTS+=("$version: 无 Clang"); return; }
    log_info "Clang: $clang_path"

    if [[ ! -f "$prep_dir/.config" ]]; then
        log_warn "未找到准备环境，执行 modules_prepare (不编完整内核)"
        if ! tools/bazel build //common:kernel_aarch64_modules_prepare 2>&1 | tail -8; then
            log_error "modules_prepare 失败"
            BUILD_RESULTS+=("$version: modules_prepare 失败")
            return
        fi
        [[ -f "$tar_rel" ]] || { log_error "未生成 $tar_rel"; BUILD_RESULTS+=("$version: 缺少 tar"); return; }
        mkdir -p "$prep_dir"
        tar -xzf "$tar_rel" -C "$prep_dir"
        log_info "已解压到 out/manual_prepare"
    else
        log_info "复用已有准备环境"
    fi

    local build_tools="$kernel_dir/build/build-tools/path/linux-x86"
    local full_path="$clang_path/bin:$build_tools:$PATH"

    if compile_module "$version" "$kernel_dir" "$prep_dir" "$full_path" "" ""; then
        handle_output "$version"
    else
        log_error "编译失败"
        BUILD_RESULTS+=("$version: 编译失败")
    fi
    clean_driver_build
}

main() {
    trap cleanup_on_exit EXIT
    local requested=("$@")
    rm -rf "$DRIVER_SRC/build" 2>/dev/null || true

    local all=()
    while IFS= read -r -d '' d; do
        local n; n=$(basename "$d")
        [[ "$n" =~ ^android.*-.* ]] && all+=("$n")
    done < <(find "$KERNELS_ROOT" -maxdepth 1 -mindepth 1 -type d -print0 2>/dev/null | sort -z)

    [[ ${#all[@]} -eq 0 ]] && { log_error "未在 $KERNELS_ROOT 发现内核目录"; exit 1; }

    log_title
    echo -e "${BLUE}发现 ${#all[@]} 个内核:${NC}"
    for k in "${all[@]}"; do
        local kind="legacy"
        [[ -f "$KERNELS_ROOT/$k/tools/bazel" ]] && kind="bazel"
        echo -e "  ${GREEN}•${NC} $k  ($kind)"
    done

    should_build() { [[ ${#requested[@]} -eq 0 ]] || printf '%s\n' "${requested[@]}" | grep -qx "$1"; }

    for v in "${all[@]}"; do
        should_build "$v" || continue
        if [[ -f "$KERNELS_ROOT/$v/tools/bazel" ]]; then
            build_bazel "$v" || true
        else
            build_legacy "$v" || true
        fi
    done

    log_title
    for r in "${BUILD_RESULTS[@]}"; do echo -e "  $r"; done
    log_title
    echo ""
    find "$DRIVER_SRC/build" -type f -name "*.ko" 2>/dev/null | sort | while read -r f; do
        echo -e "  ${GREEN}✔${NC} ${f#$DRIVER_SRC/}"
    done
    [[ -z "$(find "$DRIVER_SRC/build" -type f -name "*.ko" 2>/dev/null)" ]] && log_error "未找到任何 .ko"
    echo ""
    log_title
}

main "$@"
