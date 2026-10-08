set -euo pipefail

BUILD_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

KERNELS_ROOT="$GITHUB_WORKSPACE/kernels"
DRIVER_SRC="$GITHUB_WORKSPACE/modules"

# 需要绕过 CRC 的版本
NO_CRC_VERSIONS=(
    "android12-5.10"
    "android13-5.10"
)

GREEN='\e[32m'
RED='\e[31m'
YELLOW='\e[33m'
BLUE='\e[34m'
CYAN='\e[36m'
NC='\e[0m'

declare -a BUILD_RESULTS=()

log_info()  { echo -e "  ${GREEN}✔${NC} $*"; }
log_warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
log_error() { echo -e "  ${RED}✘${NC} $*"; }
log_step()  { echo -e "\n${CYAN}[$1]${NC} $2"; }
log_title() { echo -e "\n${BLUE}============================================================${NC}"; }

contains_version() {
    local version="$1"; shift
    local item
    for item in "$@"; do
        [[ "$version" == "$item" ]] && return 0
    done
    return 1
}

find_clang_for_kernel() {
    local kernel_dir="$1"

    local config_files=(
        "$kernel_dir/common/build.config.common"
        "$kernel_dir/common/build.config.gki.aarch64"
        "$kernel_dir/build.config.common"
        "$kernel_dir/build.config.gki.aarch64"
        "$kernel_dir/build.config.gki"
    )

    local declared_bin=""
    for cfg in "${config_files[@]}"; do
        if [[ -f "$cfg" ]]; then
            declared_bin=$(grep -E '^CLANG_PREBUILT_BIN=' "$cfg" 2>/dev/null | head -n1 | cut -d= -f2- | tr -d '"' | tr -d "'" | tr -d ' ')
            [[ -n "$declared_bin" ]] && break
        fi
    done

    if [[ -z "$declared_bin" ]]; then
        return 1
    fi

    declared_bin="${declared_bin%/bin}"

    local clang_path
    if [[ "$declared_bin" == /* ]]; then
        clang_path="$declared_bin"
    else
        clang_path="$kernel_dir/$declared_bin"
    fi

    if [[ -x "$clang_path/bin/clang" ]]; then
        echo "$clang_path"; return 0
    fi

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
    if [[ ! -d "$DRIVER_SRC" ]]; then return 1; fi
    find "$DRIVER_SRC" -type f \( \
        -name '*.o' -o -name '*.o.d' -o -name '*.mod' -o -name '*.mod.c' -o \
        -name '*.order' -o -name '*.symvers' -o -name '*.cmd' -o -name '*.usyms' \
    \) -delete 2>/dev/null || true
    find "$DRIVER_SRC" -type d -name '.tmp_versions' -prune -exec rm -rf -- {} + 2>/dev/null || true
}

cleanup_driver_build_on_exit() {
    local status=$?
    trap - EXIT
    clean_driver_build || true
    exit "$status"
}

handle_output() {
    local version="$1"

    local source_ko
    source_ko=$(find "$DRIVER_SRC" -maxdepth 1 -name "*.ko" | head -n 1)

    if [[ -z "$source_ko" || ! -f "$source_ko" ]]; then
        log_error "编译失败: 未找到 .ko 文件"
        BUILD_RESULTS+=("$version: 编译失败 (无 .ko)")
        return 1
    fi

    local ko_name
    ko_name=$(basename "$source_ko")
    local name_without_ext="${ko_name%.ko}"

    local target_dir="$DRIVER_SRC/build/$version"
    mkdir -p "$target_dir"

    local target_ko="$target_dir/${name_without_ext}.ko"

    cp "$source_ko" "$target_ko"
    rm -f "$source_ko"

    BUILD_RESULTS+=("$version: 成功")
    log_info "产物已生成: build/$version/$(basename "$target_ko")"
}

build_legacy_fast() {
    local version="$1"
    local kernel_dir="$KERNELS_ROOT/$version"

    log_title
    log_step "$version" "编译中 (Legacy 快速模式)"

    if [[ ! -d "$kernel_dir" ]]; then
        log_error "内核目录不存在: $kernel_dir"
        BUILD_RESULTS+=("$version: 目录不存在")
        return
    fi

    clean_driver_build
    cd "$kernel_dir" || return

    local common_out_dir="$(pwd)/out/$version/common"
    local kernel_build_dir="$common_out_dir/common"
    local kernel_config="$kernel_build_dir/.config"
    local kernel_src="$kernel_dir/common"

    local legacy_clang
    legacy_clang=$(find_clang_for_kernel "$kernel_dir")

    if [[ -z "$legacy_clang" || ! -x "$legacy_clang/bin/clang" ]]; then
        log_error "找不到 Clang"
        BUILD_RESULTS+=("$version: Clang 不存在")
        return 1
    fi

    local build_tools="$kernel_dir/build/build-tools/path/linux-x86"
    local FULL_PATH="$legacy_clang/bin:$build_tools:$PATH"

    if [[ ! -f "$kernel_config" ]]; then
        log_warn "未找到内核配置，执行快速准备 (gki_defconfig + modules_prepare)"

        mkdir -p "$kernel_build_dir"

        log_step "$version" "生成 gki_defconfig"
        if ! env PATH="$FULL_PATH" \
            make -C "$kernel_src" O="$kernel_build_dir" \
                ARCH=arm64 LLVM=1 LLVM_IAS=1 \
                CROSS_COMPILE=aarch64-linux-gnu- \
                CONFIG_DEBUG_INFO_BTF_MODULES= \
                gki_defconfig >/dev/null 2>&1; then
            log_error "gki_defconfig 失败"
            BUILD_RESULTS+=("$version: gki_defconfig 失败")
            return
        fi

        log_step "$version" "准备模块构建环境 (modules_prepare)"
        if ! env PATH="$FULL_PATH" \
            HOSTCFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -I$kernel_dir/prebuilts/kernel-build-tools/linux-x86/include" \
            HOSTLDFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -L$kernel_dir/prebuilts/kernel-build-tools/linux-x86/lib64 -fuse-ld=lld --rtlib=compiler-rt" \
            make -C "$kernel_src" O="$kernel_build_dir" \
                ARCH=arm64 LLVM=1 LLVM_IAS=1 \
                CONFIG_DEBUG_INFO_BTF_MODULES= \
                CROSS_COMPILE=aarch64-linux-gnu- \
                HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
                modules_prepare >/dev/null 2>&1; then
            log_error "modules_prepare 失败"
            BUILD_RESULTS+=("$version: modules_prepare 失败")
            return
        fi

        log_info "快速准备完成 (已跳过全量编译)"
    fi

    log_step "$version" "编译驱动模块"

    local symvers_file="$kernel_build_dir/Module.symvers"
    local symvers_backup=""
    local modpost_warn_param=""

    if contains_version "$version" "${NO_CRC_VERSIONS[@]}"; then
        modpost_warn_param="KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n"
        if [[ -f "$symvers_file" ]]; then
            symvers_backup="$symvers_file.no_crc_bak.$$"
            mv "$symvers_file" "$symvers_backup"
        fi
    fi

    set +e
    env PATH="$FULL_PATH" \
        HOSTCFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -I$kernel_dir/prebuilts/kernel-build-tools/linux-x86/include" \
        HOSTLDFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -L$kernel_dir/prebuilts/kernel-build-tools/linux-x86/lib64 -fuse-ld=lld --rtlib=compiler-rt" \
    make -C "$kernel_src" O="$kernel_build_dir" \
        M="$DRIVER_SRC" \
        ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        CROSS_COMPILE=aarch64-linux-gnu- \
        HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
        $modpost_warn_param \
        modules -j"$(nproc)" >/dev/null 2>&1
    local make_status=$?

    if [[ $make_status -ne 0 ]] && contains_version "$version" "${NO_CRC_VERSIONS[@]}" && fix_empty_ext_modversions; then
        log_warn "检测到空 __version_ext_names，修补后重试"
        env PATH="$FULL_PATH" \
            HOSTCFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -I$kernel_dir/prebuilts/kernel-build-tools/linux-x86/include" \
            HOSTLDFLAGS="--sysroot=$kernel_dir/build/build-tools/sysroot -L$kernel_dir/prebuilts/kernel-build-tools/linux-x86/lib64 -fuse-ld=lld --rtlib=compiler-rt" \
        make -C "$kernel_src" O="$kernel_build_dir" \
            M="$DRIVER_SRC" \
            ARCH=arm64 LLVM=1 LLVM_IAS=1 \
            CONFIG_DEBUG_INFO_BTF_MODULES= \
            CROSS_COMPILE=aarch64-linux-gnu- \
            HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
            $modpost_warn_param \
            modules -j"$(nproc)" >/dev/null 2>&1
        make_status=$?
    fi
    set -e

    if [[ -n "$symvers_backup" ]]; then
        mv "$symvers_backup" "$symvers_file"
    fi

    if [[ $make_status -ne 0 ]]; then
        log_error "编译失败"
        BUILD_RESULTS+=("$version: 编译失败")
        clean_driver_build
        return
    fi

    handle_output "$version"
    clean_driver_build
}

main() {
    trap cleanup_driver_build_on_exit EXIT

    local requested_versions=("$@")

    if [[ -d "$DRIVER_SRC/build" ]]; then
        rm -rf "$DRIVER_SRC/build"
    fi

    local all_kernels=()
    while IFS= read -r -d '' dir; do
        local name
        name=$(basename "$dir")
        if [[ "$name" =~ ^android.*-.* ]]; then
            if [[ -d "$dir/bazel-bin" ]] || [[ -f "$dir/tools/bazel" ]]; then
                continue
            fi
            all_kernels+=("$name")
        fi
    done < <(find "$KERNELS_ROOT" -maxdepth 1 -mindepth 1 -type d -print0 2>/dev/null | sort -z)

    if [[ ${#all_kernels[@]} -eq 0 ]]; then
        log_error "未在 $KERNELS_ROOT 下发现 Legacy 内核目录"
        exit 1
    fi

    log_title
    echo -e "${BLUE}Legacy 快速模式，发现 ${#all_kernels[@]} 个内核:${NC}"
    for k in "${all_kernels[@]}"; do
        echo -e "  ${GREEN}•${NC} $k"
    done

    should_build() {
        [[ ${#requested_versions[@]} -eq 0 ]] || contains_version "$1" "${requested_versions[@]}"
    }

    for version in "${all_kernels[@]}"; do
        should_build "$version" || continue
        build_legacy_fast "$version" || true
    done

    log_title
    for result in "${BUILD_RESULTS[@]}"; do
        echo -e "  $result"
    done
    log_title
    echo ""
    find "$DRIVER_SRC/build" -type f -name "*.ko" 2>/dev/null | sort | while read -r f; do
        echo -e "  ${GREEN}✔${NC} ${f#$DRIVER_SRC/}"
    done
    [[ -z "$(find "$DRIVER_SRC/build" -type f -name "*.ko" 2>/dev/null)" ]] && log_error "未找到任何 .ko 文件"
    echo ""
    log_title
}

main "$@"
