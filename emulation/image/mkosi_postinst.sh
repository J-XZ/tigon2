#!/bin/bash
set -xeuo pipefail

export DEBIAN_FRONTEND=noninteractive

# 代理处理：
# 1) 优先使用已存在的环境变量（http_proxy/https_proxy 等）
# 2) 若环境变量未设置，则尝试从 apt 配置中提取代理并回填给 curl/wget/rustup
extract_apt_proxy() {
	local proto="$1"
	local proxy_line=""

	if [ -f /etc/apt/apt.conf ]; then
		proxy_line=$(grep -E "Acquire::${proto}::Proxy" /etc/apt/apt.conf 2>/dev/null | tail -n1 || true)
	fi
	if [ -z "$proxy_line" ] && [ -d /etc/apt/apt.conf.d ]; then
		proxy_line=$(grep -hER "Acquire::${proto}::Proxy" /etc/apt/apt.conf.d 2>/dev/null | tail -n1 || true)
	fi
	if [ -n "$proxy_line" ]; then
		echo "$proxy_line" | sed -n 's/.*"\(.*\)".*/\1/p'
	fi
}

if [ -z "${http_proxy:-}" ] && [ -z "${HTTP_PROXY:-}" ]; then
	apt_http_proxy="$(extract_apt_proxy http || true)"
	if [ -n "${apt_http_proxy:-}" ]; then
		export http_proxy="$apt_http_proxy"
		export HTTP_PROXY="$apt_http_proxy"
	fi
fi

if [ -z "${https_proxy:-}" ] && [ -z "${HTTPS_PROXY:-}" ]; then
	apt_https_proxy="$(extract_apt_proxy https || true)"
	if [ -n "${apt_https_proxy:-}" ]; then
		export https_proxy="$apt_https_proxy"
		export HTTPS_PROXY="$apt_https_proxy"
	fi
fi

# 打印代理生效状态（仅打印是否存在，不打印具体地址）。
if [ -n "${http_proxy:-${HTTP_PROXY:-}}" ]; then
	echo "proxy status: HTTP proxy is set"
else
	echo "proxy status: HTTP proxy is NOT set"
fi
if [ -n "${https_proxy:-${HTTPS_PROXY:-}}" ]; then
	echo "proxy status: HTTPS proxy is set"
else
	echo "proxy status: HTTPS proxy is NOT set"
fi
if [ -n "${all_proxy:-${ALL_PROXY:-}}" ]; then
	echo "proxy status: ALL proxy is set"
else
	echo "proxy status: ALL proxy is NOT set"
fi

# 统一定义 Linux 内核版本（完整版本号，写死配置）
# 当前默认 5.19.0-50-generic
# 如需切换到 5.15.0-173-generic，直接修改下面这一行即可。
# LINUX_VERSION="5.19.0-50-generic"
LINUX_VERSION="5.15.0-173-generic"

case "${LINUX_VERSION}" in
5.15.0-173-generic | 5.19.0-50-generic) ;;

*)
	echo "ERROR: unsupported LINUX_VERSION=${LINUX_VERSION}. supported: 5.15.0-173-generic, 5.19.0-50-generic" >&2
	exit 1
	;;
esac
KERNEL_SERIES_EXPECTED="${KERNEL_SERIES_EXPECTED:-${LINUX_VERSION%%-*}}"
KIMG_PKG="linux-image-$LINUX_VERSION"

apt-get -y update

# 直接使用指定版本，无需 pick_latest_pkg
KREL="${LINUX_VERSION}"
echo "Selected kernel image package: ${KIMG_PKG}"
echo "Selected kernel release: ${KREL}"

# 检查内核系列是否匹配
if ! [[ "$KREL" =~ ^${KERNEL_SERIES_EXPECTED} ]]; then
	echo "ERROR: kernel series mismatch: detected ${KREL}, expected ${KERNEL_SERIES_EXPECTED}" >&2
	exit 1
fi

apt-get -y install --install-recommends \
	"${KIMG_PKG}" \
	"linux-headers-${KREL}" \
	"linux-modules-extra-${KREL}" \
	build-essential dkms pkg-config libelf-dev kmod bc flex bison dwarves libssl-dev libncurses-dev

# tools / cloud-tools：不同镜像源可能不全，尽量装（失败不致命）
apt-get -y install --install-recommends "linux-tools-${KREL}" || true
apt-get -y install --install-recommends "linux-cloud-tools-${KREL}" || true

# 4) hold：只 hold “实际存在且已安装”的版本化包
hold_pkgs=()
for p in \
	"linux-image-${KREL}" \
	"linux-headers-${KREL}" \
	"linux-modules-extra-${KREL}" \
	"linux-tools-${KREL}" \
	"linux-cloud-tools-${KREL}"; do
	if dpkg-query -W -f='${Status}\n' "$p" 2>/dev/null | grep -q "install ok installed"; then
		hold_pkgs+=("$p")
	fi
done

if ((${#hold_pkgs[@]})); then
	apt-mark hold "${hold_pkgs[@]}"
fi

# 5) 移除元包，避免未来 apt upgrade 拉新内核（GA/HWE 都清掉）
apt-get -y purge \
	linux-generic linux-image-generic linux-headers-generic linux-tools-generic linux-cloud-tools-generic \
	linux-generic-hwe-22.04 linux-image-generic-hwe-22.04 linux-headers-generic-hwe-22.04 linux-tools-generic-hwe-22.04 linux-cloud-tools-generic-hwe-22.04 || true

# 6) 清理其它版本内核，确保镜像里只留这一套
dpkg -l 'linux-image-[0-9]*-generic' | awk '/^ii/{print $2}' | grep -vx "linux-image-${KREL}" | xargs -r apt-get -y purge || true
dpkg -l 'linux-headers-[0-9]*-generic' | awk '/^ii/{print $2}' | grep -vx "linux-headers-${KREL}" | xargs -r apt-get -y purge || true
dpkg -l 'linux-modules-extra-[0-9]*-generic' | awk '/^ii/{print $2}' | grep -vx "linux-modules-extra-${KREL}" | xargs -r apt-get -y purge || true
dpkg -l 'linux-tools-[0-9]*-generic' | awk '/^ii/{print $2}' | grep -v "^linux-tools-${KREL}$" | xargs -r apt-get -y purge || true
dpkg -l 'linux-cloud-tools-[0-9]*-generic' | awk '/^ii/{print $2}' | grep -v "^linux-cloud-tools-${KREL}$" | xargs -r apt-get -y purge || true

apt-get -y autoremove --purge

# --- debug symbol repo ---
# Install keyring BEFORE enabling ddebs repo, and use signed-by explicitly.
apt-get -y install ubuntu-dbgsym-keyring

cat >/etc/apt/sources.list.d/ddebs.list <<EOF
deb [signed-by=/usr/share/keyrings/ubuntu-dbgsym-keyring.gpg] http://ddebs.ubuntu.com $(lsb_release -cs) main restricted universe multiverse
deb [signed-by=/usr/share/keyrings/ubuntu-dbgsym-keyring.gpg] http://ddebs.ubuntu.com $(lsb_release -cs)-updates main restricted universe multiverse
deb [signed-by=/usr/share/keyrings/ubuntu-dbgsym-keyring.gpg] http://ddebs.ubuntu.com $(lsb_release -cs)-proposed main restricted universe multiverse
EOF

update-grub

apt-get -y update || true

# azul java
apt-get -y install gnupg ca-certificates curl xz-utils
curl -s https://repos.azul.com/azul-repo.key | gpg --dearmor -o /usr/share/keyrings/azul.gpg
echo "deb [signed-by=/usr/share/keyrings/azul.gpg] https://repos.azul.com/zulu/deb stable main" | tee /etc/apt/sources.list.d/zulu.list >/dev/null

apt-get -y update || true
apt-get -y install zulu8-jdk

apt-get -y install ccache bear cmake ninja-build linux-libc-dev libc-devtools libstdc++-12-dev libjemalloc-dev libboost-all-dev libjemalloc2 libgoogle-glog0v5 libgflags2.2

# rust (system-wide via rustup; override with RUST_TOOLCHAIN=1.76.0 etc.)
: "${RUST_TOOLCHAIN:=stable}"
export RUSTUP_HOME=/usr/local/rustup
export CARGO_HOME=/usr/local/cargo
export PATH="$CARGO_HOME/bin:$PATH"
install -d -m 0755 "$RUSTUP_HOME" "$CARGO_HOME"

curl -fsSL https://sh.rustup.rs -o /tmp/rustup-init.sh
sh /tmp/rustup-init.sh -y --default-toolchain "${RUST_TOOLCHAIN}" --profile minimal --no-modify-path
rm -f /tmp/rustup-init.sh

# Make sure a default toolchain is configured for this RUSTUP_HOME.
"$CARGO_HOME/bin/rustup" default "${RUST_TOOLCHAIN}"

ln -sf "${CARGO_HOME}/bin/"* /usr/local/bin/
cat >/etc/profile.d/rust.sh <<'EOF'
export RUSTUP_HOME=/usr/local/rustup
export CARGO_HOME=/usr/local/cargo
export PATH="$CARGO_HOME/bin:$PATH"
EOF

# fish does NOT source /etc/profile.d; ensure non-interactive fish gets the same env.
install -d -m 0755 /etc/fish/conf.d
cat >/etc/fish/conf.d/rust.fish <<'EOF'
set -gx RUSTUP_HOME /usr/local/rustup
set -gx CARGO_HOME /usr/local/cargo
set -gx PATH $CARGO_HOME/bin $PATH
EOF

# llvm
wget https://apt.llvm.org/llvm.sh -O llvm.sh
chmod +x llvm.sh
# 默认仅安装 clang/clang++ 18（以及对应工具链）。
./llvm.sh 18 clang-18 clang++-18 lldb-18 lld-18 clang-tools-18 llvm-18-tools
./llvm.sh 20 clangd-20

# 默认的lldb
ln -sf /usr/bin/lldb-18 /usr/bin/lldb

# 如需其他 clang 版本，请按需手动启用以下命令示例：
# ./llvm.sh 15 clang-15 clang++-15 lldb-15 lld-15 clang-tools-15 llvm-15-tools
# ./llvm.sh 17 clang-17 clang++-17 lldb-17 lld-17 clang-tools-17 llvm-17-tools
rm llvm.sh

# gh cli
curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg
chmod go+r /usr/share/keyrings/githubcli-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" | tee /etc/apt/sources.list.d/github-cli.list >/dev/null
apt-get -y update
apt-get -y install gh

python3 -m pip install ruamel.yaml

# google test
apt install -y googletest libgtest-dev

# 清理镜像内所有 apt 代理配置（仅清理 apt proxy，不清理环境变量代理）。
purge_apt_proxy_lines() {
	local f="$1"
	[ -f "$f" ] || return 0
	sed -Ei '/Acquire::(http|https|ftp)::Proxy/Id' "$f" || true
}

purge_apt_proxy_lines /etc/apt/apt.conf
if [ -d /etc/apt/apt.conf.d ]; then
	while IFS= read -r -d '' f; do
		purge_apt_proxy_lines "$f"
	done < <(find /etc/apt/apt.conf.d -maxdepth 1 -type f -print0 2>/dev/null)
fi

if [ -d /etc/apt/apt.conf.d ]; then
	for f in /etc/apt/apt.conf.d/*; do
		if [ -f "$f" ]; then
			sed -i '/Acquire::http::Proxy/d;/Acquire::https::Proxy/d;/Acquire::ftp::Proxy/d' "$f" || true
		fi
	done
fi

for f in /etc/profile /etc/bash.bashrc /root/.bashrc /root/.profile /root/.bash_profile /root/.bash_login; do
	if [ -f "$f" ]; then
		sed -i '/http_proxy/d;/https_proxy/d;/HTTP_PROXY/d;/HTTPS_PROXY/d;/all_proxy/d;/ALL_PROXY/d;/no_proxy/d;/NO_PROXY/d' "$f" || true
	fi
done
if [ -d /etc/profile.d ]; then
	for f in /etc/profile.d/*.sh; do
		if [ -f "$f" ]; then
			sed -i '/http_proxy/d;/https_proxy/d;/HTTP_PROXY/d;/HTTPS_PROXY/d;/all_proxy/d;/ALL_PROXY/d;/no_proxy/d;/NO_PROXY/d' "$f" || true
		fi
	done
fi

unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY no_proxy NO_PROXY || true

# 清理旧的系统级代理文件；若本次宿主无代理，保持镜像无代理环境。
rm -f /etc/environment.d/99-cxlkv-proxy.conf || true
rm -f /etc/profile.d/99-proxy.sh || true
rm -f /etc/fish/conf.d/99-proxy.fish || true

# 从宿主机导出的代理片段中提取变量值（支持 `export KEY=...` 与 `KEY=...`）。
extract_proxy_value_from_file() {
	local file="$1"
	local key="$2"
	[ -s "$file" ] || return 0
	awk -v key="$key" '
	BEGIN { found = 0 }
	{
		line = $0
		gsub(/\r/, "", line)
		if (line ~ "^[[:space:]]*(export[[:space:]]+)?" key "[[:space:]]*=") {
			sub("^[[:space:]]*(export[[:space:]]+)?(" key ")[[:space:]]*=[[:space:]]*", "", line)
			sub(/[[:space:]]+#.*$/, "", line)
			if ((line ~ /^".*"$/) || (line ~ /^'\''.*'\''$/)) {
				line = substr(line, 2, length(line) - 2)
			}
			print line
			found = 1
			exit
		}
	}
	END { if (!found) exit 0 }
	' "$file" || true
}

strip_proxy_keys_from_etc_environment() {
	[ -f /etc/environment ] || return 0
	sed -i '/^http_proxy=/d;/^HTTP_PROXY=/d;/^https_proxy=/d;/^HTTPS_PROXY=/d;/^no_proxy=/d;/^NO_PROXY=/d;/^all_proxy=/d;/^ALL_PROXY=/d' \
		/etc/environment || true
}

# 若宿主机导出了代理变量，
# 在镜像收尾时回填到系统级环境与 shell 配置，
# 让非交互脚本也可直接继承代理。
host_bashrc_proxy_exports="/etc/cxlkv_host_bashrc_proxy_exports"
environment_proxy_snippet="/etc/cxlkv_environment_proxy.snippet"

host_http_proxy=""
host_https_proxy=""
host_no_proxy=""

if [ -s "$host_bashrc_proxy_exports" ]; then
	host_http_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "http_proxy")"
	[ -z "${host_http_proxy:-}" ] && host_http_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "HTTP_PROXY")"
	host_https_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "https_proxy")"
	[ -z "${host_https_proxy:-}" ] && host_https_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "HTTPS_PROXY")"
	host_no_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "no_proxy")"
	[ -z "${host_no_proxy:-}" ] && host_no_proxy="$(extract_proxy_value_from_file "$host_bashrc_proxy_exports" "NO_PROXY")"
fi

if [ -z "${host_http_proxy:-}" ] && [ -z "${host_https_proxy:-}" ] && [ -z "${host_no_proxy:-}" ] && [ -s "$environment_proxy_snippet" ]; then
	host_http_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "http_proxy")"
	[ -z "${host_http_proxy:-}" ] && host_http_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "HTTP_PROXY")"
	host_https_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "https_proxy")"
	[ -z "${host_https_proxy:-}" ] && host_https_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "HTTPS_PROXY")"
	host_no_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "no_proxy")"
	[ -z "${host_no_proxy:-}" ] && host_no_proxy="$(extract_proxy_value_from_file "$environment_proxy_snippet" "NO_PROXY")"
fi

if [ -n "${host_http_proxy:-}" ] || [ -n "${host_https_proxy:-}" ] || [ -n "${host_no_proxy:-}" ]; then
	# 1) /etc/environment：优先使用构建阶段写入的片段（与 make_vm_img 字节一致），否则回退到解析值。
	strip_proxy_keys_from_etc_environment
	touch /etc/environment
	if [ -s "$environment_proxy_snippet" ]; then
		cat "$environment_proxy_snippet" >>/etc/environment
	else
		{
			[ -n "${host_http_proxy:-}" ] && echo "http_proxy=\"${host_http_proxy}\"" && echo "HTTP_PROXY=\"${host_http_proxy}\""
			[ -n "${host_https_proxy:-}" ] && echo "https_proxy=\"${host_https_proxy}\"" && echo "HTTPS_PROXY=\"${host_https_proxy}\""
			[ -n "${host_no_proxy:-}" ] && echo "no_proxy=\"${host_no_proxy}\"" && echo "NO_PROXY=\"${host_no_proxy}\""
		} >>/etc/environment
	fi

	# 1.1) /etc/environment.d：systemd 管理的非交互任务也可继承代理。
	install -d -m 0755 /etc/environment.d
	cat >/etc/environment.d/99-cxlkv-proxy.conf <<EOF
$([ -n "${host_http_proxy:-}" ] && echo "http_proxy=${host_http_proxy}")
$([ -n "${host_http_proxy:-}" ] && echo "HTTP_PROXY=${host_http_proxy}")
$([ -n "${host_https_proxy:-}" ] && echo "https_proxy=${host_https_proxy}")
$([ -n "${host_https_proxy:-}" ] && echo "HTTPS_PROXY=${host_https_proxy}")
$([ -n "${host_no_proxy:-}" ] && echo "no_proxy=${host_no_proxy}")
$([ -n "${host_no_proxy:-}" ] && echo "NO_PROXY=${host_no_proxy}")
EOF

	# 1.2) /etc/profile.d：登录 shell 统一继承代理（含大小写变量）。
	cat >/etc/profile.d/99-proxy.sh <<EOF
$([ -n "${host_http_proxy:-}" ] && echo "export http_proxy=\"${host_http_proxy}\"")
$([ -n "${host_http_proxy:-}" ] && echo "export HTTP_PROXY=\"${host_http_proxy}\"")
$([ -n "${host_https_proxy:-}" ] && echo "export https_proxy=\"${host_https_proxy}\"")
$([ -n "${host_https_proxy:-}" ] && echo "export HTTPS_PROXY=\"${host_https_proxy}\"")
$([ -n "${host_no_proxy:-}" ] && echo "export no_proxy=\"${host_no_proxy}\"")
$([ -n "${host_no_proxy:-}" ] && echo "export NO_PROXY=\"${host_no_proxy}\"")
EOF
	chmod 0644 /etc/profile.d/99-proxy.sh

	# 2) root bash：保持交互与 ssh 登录 shell 体验一致。
	touch /root/.bashrc
	{
		echo ""
		echo "# Added by mkosi_postinst from host ~/.bashrc"
		[ -n "${host_http_proxy:-}" ] && echo "export http_proxy=\"${host_http_proxy}\"" && echo "export HTTP_PROXY=\"${host_http_proxy}\""
		[ -n "${host_https_proxy:-}" ] && echo "export https_proxy=\"${host_https_proxy}\"" && echo "export HTTPS_PROXY=\"${host_https_proxy}\""
		[ -n "${host_no_proxy:-}" ] && echo "export no_proxy=\"${host_no_proxy}\"" && echo "export NO_PROXY=\"${host_no_proxy}\""
	} >>/root/.bashrc

	# 3) fish：补齐 fish 用户态行为，避免 shell 间不一致。
	install -d -m 0755 /etc/fish/conf.d
	cat >/etc/fish/conf.d/99-proxy.fish <<EOF
$([ -n "${host_http_proxy:-}" ] && echo "set -gx http_proxy \"${host_http_proxy}\"")
$([ -n "${host_http_proxy:-}" ] && echo "set -gx HTTP_PROXY \"${host_http_proxy}\"")
$([ -n "${host_https_proxy:-}" ] && echo "set -gx https_proxy \"${host_https_proxy}\"")
$([ -n "${host_https_proxy:-}" ] && echo "set -gx HTTPS_PROXY \"${host_https_proxy}\"")
$([ -n "${host_no_proxy:-}" ] && echo "set -gx no_proxy \"${host_no_proxy}\"")
$([ -n "${host_no_proxy:-}" ] && echo "set -gx NO_PROXY \"${host_no_proxy}\"")
EOF
fi

rm -f "$environment_proxy_snippet" || true

# 确保登录 shell 会加载 /root/.bashrc（部分镜像里 root 存在 .bash_profile 且不自动 source .bashrc）。
touch /root/.bash_profile
if ! grep -q 'source /root/.bashrc' /root/.bash_profile 2>/dev/null &&
	! grep -q '\. /root/.bashrc' /root/.bash_profile 2>/dev/null &&
	! grep -q 'source ~/.bashrc' /root/.bash_profile 2>/dev/null &&
	! grep -q '\. ~/.bashrc' /root/.bash_profile 2>/dev/null; then
	{
		echo ""
		echo "# Added by mkosi_postinst: load /root/.bashrc for login shells"
		echo "if [ -f /root/.bashrc ]; then"
		echo "  . /root/.bashrc"
		echo "fi"
	} >>/root/.bash_profile
fi

rm -f "$host_bashrc_proxy_exports" || true
