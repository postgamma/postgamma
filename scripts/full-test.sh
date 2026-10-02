#!/usr/bin/env bash
# Provision a supported Linux host and run the complete source validation.

set -Eeuo pipefail
unset LANGUAGE LC_ADDRESS LC_COLLATE LC_CTYPE LC_IDENTIFICATION \
	LC_MEASUREMENT LC_MESSAGES LC_MONETARY LC_NAME LC_NUMERIC LC_PAPER \
	LC_TELEPHONE LC_TIME
export LC_ALL=C
export LANG=C

readonly SCRIPT_NAME=${0##*/}
readonly MICROMAMBA_VERSION=2.8.1-0
readonly MICROMAMBA_SHA256=9689782d863c05a1bf5d2d371ba527104e7a4eb4310c1637d8653b751aed9c82
readonly MICROMAMBA_URL="https://github.com/mamba-org/micromamba-releases/releases/download/${MICROMAMBA_VERSION}/micromamba-linux-64"
readonly GIT_VERSION=2.43.7
readonly GIT_SHA256=657e2374455d9e62f6cdb3e7c55d867b6db5404d744e97e112cc5b0db687a19f
readonly GIT_URL="https://www.kernel.org/pub/software/scm/git/git-${GIT_VERSION}.tar.xz"
readonly IPC_RUN_VERSION=0.94
readonly IPC_RUN_SHA256=2eb336c91a2b7ea61f98e5b2282d91020d39a484f16041e2365ffd30f8a5605b
readonly IPC_RUN_URL="https://cpan.metacpan.org/authors/id/T/TO/TODDR/IPC-Run-${IPC_RUN_VERSION}.tar.gz"
readonly TOOLCHAIN_LOCK_RELATIVE=buildsys/toolchains/full-test-linux-64.lock
readonly REQUIRED_NOFILE=1048576

mode=run
for argument in "$@"; do
	case "$argument" in
	--plan)
		mode=plan
		;;
	--prepare-only)
		mode=prepare
		;;
	-h | --help)
		cat <<'EOF'
Usage: ./scripts/full-test.sh [--plan | --prepare-only]

Provision a supported x86-64 Linux host and run complete-source-check as the
current unprivileged user.  --plan prints the detected installation plan
without changing the host.  --prepare-only installs and verifies the
environment but does not start the test. Privileged package, swap, and
process-limit commands use sudo without changing the identity that runs builds
and tests.

Useful environment overrides:
  POSTGAMMA_JOBS             nested build concurrency (default: memory-aware)
  POSTGAMMA_WORK_ROOT        managed source/build directory
  POSTGAMMA_STATE_ROOT       reusable user-owned toolchain directory
  POSTGAMMA_TOOLCHAIN        checksum-pinned toolchain prefix
  POSTGAMMA_GIT_PREFIX       locally built Git prefix
  POSTGAMMA_REPOSITORY       main repository used outside a checkout
  POSTGAMMA_REPOSITORY_FALLBACK
                             optional fallback main repository
  POSTGAMMA_REF              branch, tag, or commit used outside a checkout
  POSTGAMMA_SKIP_PACKAGES=1  skip the operating-system package installation
  POSTGAMMA_CREATE_SWAP      auto, yes, or no (default: auto)
  POSTGAMMA_SWAP_SWAPPINESS  temporary low-memory swap policy (default: 60)
  PG_REPOSITORY              optional PostgreSQL submodule mirror
  PG_REPOSITORY_FALLBACK     optional fallback PostgreSQL mirror
  PGVECTOR_REPOSITORY        optional pgvector submodule mirror
  PGVECTOR_REPOSITORY_FALLBACK
                             optional fallback pgvector mirror
EOF
		exit 0
		;;
	*)
		echo "${SCRIPT_NAME}: unknown argument: ${argument}" >&2
		exit 2
		;;
	esac
done

log()
{
	printf '[postgamma-full-test] %s\n' "$*"
}

fail()
{
	printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2
	exit 2
}

require_safe_absolute_path()
{
	local label=$1
	local path=$2

	case "$path" in
	/*) ;;
	*) fail "$label must be an absolute path: $path" ;;
	esac
	case "$path" in
	/ | /bin | /boot | /dev | /etc | /home | /lib | /lib64 | /opt | /proc | /root | /run | /sbin | /sys | /tmp | /usr | /var)
		fail "$label is too broad: $path"
		;;
	esac
	[[ "$path" != *[[:space:]]* ]] || fail "$label must not contain whitespace: $path"
}

script_path=${BASH_SOURCE[0]:-}
checkout_root=
if [[ -n "$script_path" && "$script_path" != /dev/fd/* ]]; then
	candidate_root=$(cd "$(dirname "$script_path")/.." 2>/dev/null && pwd -P || true)
	if [[ -n "$candidate_root" && -f "$candidate_root/Makefile" ]] &&
		[[ -d "$candidate_root/.git" || -f "$candidate_root/.git" ]]; then
		checkout_root=$candidate_root
	fi
fi

os_release=${POSTGAMMA_OS_RELEASE:-/etc/os-release}
[[ -r "$os_release" ]] || fail "cannot read operating-system metadata: $os_release"

ID=
ID_LIKE=
VERSION_ID=
NAME=
# /etc/os-release is a distribution-owned shell-compatible data file.
# shellcheck disable=SC1090
. "$os_release"
os_id=${ID,,}
os_like=${ID_LIKE,,}
os_version=${VERSION_ID:-unknown}
os_name=${NAME:-$os_id}

identities=" $os_id $os_like "
package_family=
package_manager=
case "$identities" in
*" debian "* | *" ubuntu "*)
	package_family=apt
	package_manager=apt-get
	;;
*" fedora "* | *" rhel "* | *" centos "* | *" rocky "* | *" almalinux "*)
	package_family=rpm
	if [[ -n ${POSTGAMMA_PACKAGE_MANAGER:-} ]]; then
		package_manager=$POSTGAMMA_PACKAGE_MANAGER
	elif command -v dnf >/dev/null 2>&1; then
		package_manager=dnf
	else
		package_manager=yum
	fi
	;;
*" suse "* | *" opensuse "* | *" opensuse-leap "*)
	package_family=zypper
	package_manager=zypper
	;;
*)
	fail "unsupported Linux distribution: ID=$os_id ID_LIKE=${ID_LIKE:-none}; supported package families are apt, dnf/yum, and zypper"
	;;
esac

architecture=$(uname -m)
case "$architecture" in
x86_64 | amd64) ;;
*) fail "unsupported architecture: $architecture (x86-64 is required)" ;;
esac

apt_packages=(
	bash bison build-essential ca-certificates coreutils curl diffutils file
	findutils flex git libcurl4-openssl-dev libreadline-dev patch perl pkg-config
	strace tar util-linux xz-utils zlib1g-dev
)
rpm_legacy_packages=(
	bash bison ca-certificates coreutils curl diffutils file findutils flex gcc
	gcc-c++ git libcurl-devel make patch perl pkgconfig readline-devel strace tar
	util-linux which xz zlib-devel
)
rpm_packages=(
	bash bison ca-certificates coreutils curl diffutils file findutils flex gcc
	gcc-c++ git libcurl-devel make patch perl pkgconf-pkg-config readline-devel
	strace tar util-linux which xz zlib-devel
)
zypper_packages=(
	bash bison ca-certificates coreutils curl diffutils file findutils flex gcc
	gcc-c++ git libcurl-devel make patch perl pkgconf-pkg-config readline-devel
	strace tar util-linux which xz zlib-devel
)

packages=()
case "$package_family" in
apt) packages=("${apt_packages[@]}") ;;
rpm)
	major=${os_version%%.*}
	if [[ "$major" =~ ^[0-9]+$ && "$major" -le 7 ]]; then
		packages=("${rpm_legacy_packages[@]}")
	else
		packages=("${rpm_packages[@]}")
	fi
	;;
zypper) packages=("${zypper_packages[@]}") ;;
esac

((EUID != 0)) || fail "do not run as root; switch to a regular user and run ./$SCRIPT_NAME"

test_user=$(id -un)
test_home=${HOME:-}
[[ -n "$test_home" && -d "$test_home" ]] || fail "the current user has no accessible home directory"
if [[ -n "$checkout_root" ]]; then
	default_work_root=$checkout_root/build/full-test
else
	default_work_root=$(pwd -P)/build/full-test
fi
work_root=${POSTGAMMA_WORK_ROOT:-$default_work_root}
state_root=${POSTGAMMA_STATE_ROOT:-$work_root/state}
if [[ -n "$checkout_root" ]]; then
	source_dir=$checkout_root
else
	source_dir=$work_root/source
fi
toolchain=${POSTGAMMA_TOOLCHAIN:-$state_root/toolchain}
git_prefix=${POSTGAMMA_GIT_PREFIX:-$state_root/git}
system_bridge=${POSTGAMMA_SYSTEM_BRIDGE:-$state_root/system-cc}
micromamba_root=${POSTGAMMA_MICROMAMBA_ROOT:-$state_root/micromamba}
perl_modules=${POSTGAMMA_PERL_MODULES:-$state_root/perl}
tools_root=${POSTGAMMA_TOOLS_ROOT:-$state_root/tools}
swap_file=${POSTGAMMA_SWAP_FILE:-/var/tmp/postgamma-full-test-${UID}.swap}
repository=${POSTGAMMA_REPOSITORY:-https://github.com/postgamma/postgamma.git}
repository_fallback=${POSTGAMMA_REPOSITORY_FALLBACK:-}
repository_ref=${POSTGAMMA_REF:-main}
swap_mode=${POSTGAMMA_CREATE_SWAP:-auto}
swap_swappiness=${POSTGAMMA_SWAP_SWAPPINESS:-60}
session_timeout=${SESSION_EXECUTOR_TIMEOUT_SECONDS:-3600}
session_request_timeout=${SESSION_EXECUTOR_REQUEST_TIMEOUT_SECONDS:-300}
logical_management_timeout=${LOGICAL_MANAGEMENT_TIMEOUT_SECONDS:-1800}

require_safe_absolute_path POSTGAMMA_WORK_ROOT "$work_root"
require_safe_absolute_path POSTGAMMA_STATE_ROOT "$state_root"
require_safe_absolute_path POSTGAMMA_TOOLCHAIN "$toolchain"
require_safe_absolute_path POSTGAMMA_GIT_PREFIX "$git_prefix"
require_safe_absolute_path POSTGAMMA_SYSTEM_BRIDGE "$system_bridge"
require_safe_absolute_path POSTGAMMA_MICROMAMBA_ROOT "$micromamba_root"
require_safe_absolute_path POSTGAMMA_PERL_MODULES "$perl_modules"
require_safe_absolute_path POSTGAMMA_TOOLS_ROOT "$tools_root"
require_safe_absolute_path POSTGAMMA_SWAP_FILE "$swap_file"

memory_kib=$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo 2>/dev/null || true)
memory_kib=${memory_kib:-0}
cpu_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
if [[ -n ${POSTGAMMA_JOBS:-} ]]; then
	jobs=$POSTGAMMA_JOBS
elif ((memory_kib < 3145728)); then
	jobs=1
elif ((cpu_count > 4)); then
	jobs=4
else
	jobs=$cpu_count
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "POSTGAMMA_JOBS must be a positive integer"
[[ "$session_timeout" =~ ^[1-9][0-9]*$ ]] || fail "SESSION_EXECUTOR_TIMEOUT_SECONDS must be a positive integer"
[[ "$session_request_timeout" =~ ^[1-9][0-9]*$ ]] || fail "SESSION_EXECUTOR_REQUEST_TIMEOUT_SECONDS must be a positive integer"
[[ "$logical_management_timeout" =~ ^[1-9][0-9]*$ ]] || fail "LOGICAL_MANAGEMENT_TIMEOUT_SECONDS must be a positive integer"
case "$swap_mode" in
auto | yes | no) ;;
*) fail "POSTGAMMA_CREATE_SWAP must be auto, yes, or no" ;;
esac
[[ "$swap_swappiness" =~ ^[1-9][0-9]?$|^100$ ]] ||
	fail "POSTGAMMA_SWAP_SWAPPINESS must be an integer from 1 through 100"

if [[ "$mode" == plan ]]; then
	printf 'os=%s %s\n' "$os_name" "$os_version"
	printf 'architecture=%s\n' "$architecture"
	printf 'package_family=%s\n' "$package_family"
	printf 'package_manager=%s\n' "$package_manager"
	printf 'packages=%s\n' "${packages[*]}"
	printf 'execution_user=%s\n' "$test_user"
	printf 'work_root=%s\n' "$work_root"
	printf 'state_root=%s\n' "$state_root"
	printf 'toolchain=%s\n' "$toolchain"
	printf 'workflow_requirements=buildsys/requirements.txt\n'
	printf 'git_prefix=%s\n' "$git_prefix"
	printf 'perl_modules=%s\n' "$perl_modules"
	printf 'jobs=%s\n' "$jobs"
	printf 'swap=%s\n' "$swap_mode"
	printf 'swap_swappiness=%s\n' "$swap_swappiness"
	printf 'session_executor_timeout_seconds=%s\n' "$session_timeout"
	printf 'session_executor_request_timeout_seconds=%s\n' "$session_request_timeout"
	printf 'logical_management_timeout_seconds=%s\n' "$logical_management_timeout"
	if [[ -n "$checkout_root" ]]; then
		printf 'source=current-checkout\n'
	else
		printf 'source=%s\n' "$repository"
		if [[ -n "$repository_fallback" ]]; then
			printf 'source_fallback=%s\n' "$repository_fallback"
		fi
		printf 'ref=%s\n' "$repository_ref"
	fi
	printf 'postgres_source=%s\n' "${PG_REPOSITORY:-submodule-default}"
	printf 'postgres_source_fallback=%s\n' "${PG_REPOSITORY_FALLBACK:-none}"
	printf 'pgvector_source=%s\n' "${PGVECTOR_REPOSITORY:-submodule-default}"
	printf 'pgvector_source_fallback=%s\n' "${PGVECTOR_REPOSITORY_FALLBACK:-none}"
	exit 0
fi

sudo_command=$(command -v sudo || true)
sudo_keepalive_pid=
sudo_owner_pid=$$

ensure_sudo_session()
{
	[[ -n "$sudo_command" ]] || fail "sudo is required for package or swap setup; install prerequisites manually and use POSTGAMMA_SKIP_PACKAGES=1 POSTGAMMA_CREATE_SWAP=no"
	if [[ -z "$sudo_keepalive_pid" ]]; then
		"$sudo_command" -v || fail "sudo authentication is required before provisioning"
		(
			while sleep 60 && kill -0 "$sudo_owner_pid" 2>/dev/null; do
				"$sudo_command" -n -v >/dev/null 2>&1 || break
			done
		) &
		sudo_keepalive_pid=$!
		trap cleanup EXIT
	fi
}

run_privileged()
{
	ensure_sudo_session
	"$sudo_command" -n -- "$@"
}

install_packages()
{
	if [[ ${POSTGAMMA_SKIP_PACKAGES:-0} == 1 ]]; then
		log "skipping operating-system packages by request"
		return
	fi
	log "installing ${package_family} dependencies with ${package_manager}"
	case "$package_family" in
	apt)
		run_privileged env DEBIAN_FRONTEND=noninteractive apt-get update
		run_privileged env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
		;;
	rpm)
		run_privileged "$package_manager" install -y "${packages[@]}"
		;;
	zypper)
		run_privileged zypper --non-interactive install -y "${packages[@]}"
		;;
	esac
}

download_file()
{
	local destination=$1
	local url=$2
	local description=$3
	local attempt
	local -a curl_options=(--fail --location)

	# Older curl releases do not retry every transport error.  In particular,
	# curl 7.61 can return 92 after an interrupted HTTP/2 stream without using
	# --retry.  Prefer HTTP/1.1 when the installed curl supports the option and
	# retry the complete command so every nonzero transport result is covered.
	if curl --http1.1 --version >/dev/null 2>&1; then
		curl_options+=(--http1.1)
	fi
	for attempt in 1 2 3 4; do
		if curl "${curl_options[@]}" \
			--output "$destination" "$url"; then
			return
		fi
		if ((attempt < 4)); then
			log "$description download attempt $attempt failed; retrying" >&2
			sleep $((attempt * 2))
		fi
	done
	fail "could not download $description after 4 attempts"
}

prepare_swap()
{
	local swap_kib available_kib desired_kib
	swap_kib=$(awk '/^SwapTotal:/ { print $2; exit }' /proc/meminfo 2>/dev/null || echo 0)
	desired_kib=$((4 * 1024 * 1024))
	if [[ "$swap_mode" == no ]]; then
		return
	fi
	if [[ "$swap_mode" == auto ]] && ((memory_kib >= 4 * 1024 * 1024)); then
		return
	fi
	if ((swap_kib < desired_kib)); then
		if awk '{print $1}' /proc/swaps | grep -Fx "$swap_file" >/dev/null 2>&1; then
			log "reusing active swap file $swap_file"
		else
			if [[ ! -f "$swap_file" ]]; then
				available_kib=$(df -Pk "$(dirname "$swap_file")" | awk 'NR == 2 { print $4 }')
				((available_kib >= desired_kib + 2 * 1024 * 1024)) || fail "insufficient disk space to create the 4 GiB low-memory swap file"
				log "creating 4 GiB low-memory swap file $swap_file"
				if ! run_privileged fallocate -l 4G "$swap_file" 2>/dev/null; then
					run_privileged dd if=/dev/zero of="$swap_file" bs=1M count=4096
				fi
			fi
			run_privileged chmod 0600 "$swap_file"
			run_privileged mkswap "$swap_file" >/dev/null
			run_privileged swapon "$swap_file"
		fi
	else
		log "reusing $((swap_kib / 1024)) MiB of active swap"
	fi
	prepare_swap_policy
}

original_global_swappiness=
original_cgroup_swappiness=
swappiness_cgroup_file=

restore_swap_policy()
{
	if [[ -n "$original_cgroup_swappiness" ]]; then
		printf '%s\n' "$original_cgroup_swappiness" |
			"$sudo_command" -n -- tee "$swappiness_cgroup_file" >/dev/null ||
			log "warning: could not restore $swappiness_cgroup_file"
	fi
	if [[ -n "$original_global_swappiness" ]]; then
		"$sudo_command" -n -- sysctl -q -w \
			"vm.swappiness=$original_global_swappiness" ||
			log "warning: could not restore vm.swappiness"
	fi
}

cleanup()
{
	restore_swap_policy
	if [[ -n "$sudo_keepalive_pid" ]]; then
		kill "$sudo_keepalive_pid" 2>/dev/null || true
		wait "$sudo_keepalive_pid" 2>/dev/null || true
	fi
}

prepare_swap_policy()
{
	local current cgroup_path
	current=$(cat /proc/sys/vm/swappiness 2>/dev/null || true)
	[[ "$current" =~ ^[0-9]+$ ]] || fail "cannot read vm.swappiness"
	if ((current < swap_swappiness)); then
		original_global_swappiness=$current
		log "temporarily setting vm.swappiness from $current to $swap_swappiness"
		run_privileged sysctl -q -w "vm.swappiness=$swap_swappiness"
	fi

	cgroup_path=$(awk -F: '$2 ~ /(^|,)memory(,|$)/ { print $3; exit }' /proc/self/cgroup 2>/dev/null || true)
	if [[ -n "$cgroup_path" ]]; then
		swappiness_cgroup_file="/sys/fs/cgroup/memory${cgroup_path%/}/memory.swappiness"
		if [[ -f "$swappiness_cgroup_file" ]]; then
			current=$(cat "$swappiness_cgroup_file" 2>/dev/null || true)
			[[ "$current" =~ ^[0-9]+$ ]] || fail "cannot read $swappiness_cgroup_file"
			if ((current < swap_swappiness)); then
				original_cgroup_swappiness=$current
				log "temporarily setting cgroup swappiness from $current to $swap_swappiness"
				printf '%s\n' "$swap_swappiness" |
					run_privileged tee "$swappiness_cgroup_file" >/dev/null
			fi
		fi
	fi
}

reset_managed_directory()
{
	local marker=$work_root/.postgamma-full-test-root
	if [[ -e "$work_root" ]]; then
		[[ -f "$marker" ]] || fail "refusing to replace unmarked work directory: $work_root"
		rm -rf -- "$work_root/source" "$work_root/build" "$work_root/input" "$work_root/run"
	else
		mkdir -p "$work_root"
	fi
	printf 'managed by %s\n' "$SCRIPT_NAME" >"$marker"
	mkdir -p "$work_root/input" "$work_root/run"
}

stage_local_repository()
{
	local variable=$1
	local name=$2
	local value=${!variable:-}
	local source destination

	[[ -n "$value" ]] || return 0
	case "$value" in
	file:///*) source=${value#file://} ;;
	/*) source=$value ;;
	*)
		[[ -e "$value" ]] || return 0
		source=$value
		;;
	esac
	[[ -e "$source" ]] || fail "$variable local repository does not exist: $source"
	source=$(readlink -f -- "$source")
	destination=$work_root/input/$name
	log "staging local $name repository for the current user"
	cp -a -- "$source" "$destination"
	printf -v "$variable" '%s' "$destination"
	export "$variable"
}

stage_repository_inputs()
{
	POSTGAMMA_REPOSITORY=$repository
	POSTGAMMA_REPOSITORY_FALLBACK=$repository_fallback
	PG_REPOSITORY=${PG_REPOSITORY:-}
	PG_REPOSITORY_FALLBACK=${PG_REPOSITORY_FALLBACK:-}
	PGVECTOR_REPOSITORY=${PGVECTOR_REPOSITORY:-}
	PGVECTOR_REPOSITORY_FALLBACK=${PGVECTOR_REPOSITORY_FALLBACK:-}
	stage_local_repository POSTGAMMA_REPOSITORY postgamma-primary
	stage_local_repository POSTGAMMA_REPOSITORY_FALLBACK postgamma-fallback
	stage_local_repository PG_REPOSITORY postgres-primary
	stage_local_repository PG_REPOSITORY_FALLBACK postgres-fallback
	stage_local_repository PGVECTOR_REPOSITORY pgvector-primary
	stage_local_repository PGVECTOR_REPOSITORY_FALLBACK pgvector-fallback
	repository=$POSTGAMMA_REPOSITORY
	repository_fallback=$POSTGAMMA_REPOSITORY_FALLBACK
}

prepare_source()
{
	local revision=
	local candidate label

	if [[ -n "$checkout_root" ]]; then
		log "using the current clean source checkout"
		if [[ -n $(cd "$checkout_root" && git status --porcelain --untracked-files=no) ]]; then
			fail "the current checkout has tracked changes; commit or stash them before full validation"
		fi
		(cd "$source_dir" && git rev-parse --verify HEAD >/dev/null)
	else
		log "preparing an isolated source checkout"
		for label in primary fallback; do
			if [[ "$label" == primary ]]; then
				candidate=$repository
			else
				candidate=$repository_fallback
			fi
			[[ -n "$candidate" ]] || continue
			log "cloning the $label main repository"
			if git clone "$candidate" "$source_dir"; then
				break
			fi
			rm -rf -- "$source_dir"
		done
		[[ -d "$source_dir/.git" ]] || fail "cannot clone the primary or fallback main repository"
		if [[ -n "$repository_ref" ]]; then
			if revision=$(cd "$source_dir" && git rev-parse --verify "${repository_ref}^{commit}" 2>/dev/null); then
				:
			elif revision=$(cd "$source_dir" && git rev-parse --verify "origin/${repository_ref}^{commit}" 2>/dev/null); then
				:
			else
				fail "cannot resolve POSTGAMMA_REF as a branch, tag, or commit: $repository_ref"
			fi
			(cd "$source_dir" && git checkout --detach "$revision")
		fi
	fi
	[[ -f "$source_dir/$TOOLCHAIN_LOCK_RELATIVE" ]] || fail "source checkout omits $TOOLCHAIN_LOCK_RELATIVE"
}

install_micromamba()
{
	local micromamba=$tools_root/micromamba
	local temporary=$tools_root/micromamba.download

	mkdir -p "$tools_root"
	if [[ -f "$micromamba" ]] && printf '%s  %s\n' "$MICROMAMBA_SHA256" "$micromamba" | sha256sum --check --status; then
		printf '%s' "$micromamba"
		return
	fi
	log "downloading checksum-pinned micromamba $MICROMAMBA_VERSION" >&2
	download_file "$temporary" "$MICROMAMBA_URL" "micromamba $MICROMAMBA_VERSION"
	printf '%s  %s\n' "$MICROMAMBA_SHA256" "$temporary" | sha256sum --check --status || fail "micromamba checksum mismatch"
	chmod 0755 "$temporary"
	mv -f "$temporary" "$micromamba"
	printf '%s' "$micromamba"
}

install_toolchain()
{
	local lock=$source_dir/$TOOLCHAIN_LOCK_RELATIVE
	local lock_sha marker ownership_marker micromamba
	lock_sha=$(sha256sum "$lock" | awk '{print $1}')
	marker=$toolchain/.postgamma-full-test-lock
	ownership_marker=${toolchain}.postgamma-managed
	if [[ -x "$toolchain/bin/python3" && -x "$toolchain/bin/make" && -x "$toolchain/bin/clang" && -f "$marker" && $(cat "$marker") == "$lock_sha" ]]; then
		log "reusing checksum-matched toolchain $toolchain"
		printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
	else
		micromamba=$(install_micromamba)
		log "installing checksum-pinned Python, GNU Make, and LLVM toolchain"
		if [[ -e "$toolchain" ]]; then
			[[ -f "$ownership_marker" ]] || fail "refusing to replace unmarked toolchain directory: $toolchain"
			rm -rf -- "$toolchain"
		fi
		printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
		mkdir -p "$micromamba_root"
		MAMBA_ROOT_PREFIX="$micromamba_root" "$micromamba" create --yes --prefix "$toolchain" --file "$lock"
		printf '%s\n' "$lock_sha" >"$marker"
	fi
}

install_workflow_dependencies()
{
	log "installing workflow validation dependencies in the managed toolchain"
	"$toolchain/bin/python3" -m pip install --disable-pip-version-check \
		--requirement "$source_dir/buildsys/requirements.txt"
}

install_git()
{
	local archive=$tools_root/git-${GIT_VERSION}.tar.xz
	local source=$tools_root/git-${GIT_VERSION}
	local marker=$git_prefix/.postgamma-git-version
	local ownership_marker=${git_prefix}.postgamma-managed
	local system_cc
	local -a make_arguments

	mkdir -p "$tools_root"
	if [[ -f "$marker" && $(cat "$marker") == "$GIT_VERSION" ]] &&
		[[ $("$git_prefix/bin/git" --version) == "git version $GIT_VERSION" ]]; then
		log "reusing locally built Git $GIT_VERSION"
		printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
		return
	fi
	if [[ -e "$git_prefix" ]]; then
		[[ -f "$ownership_marker" ]] || fail "refusing to replace unmarked Git directory: $git_prefix"
		rm -rf -- "$git_prefix"
	fi
	printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
	if [[ ! -f "$archive" ]] || ! printf '%s  %s\n' "$GIT_SHA256" "$archive" | sha256sum --check --status; then
		log "downloading checksum-pinned Git $GIT_VERSION"
		download_file "$archive.download" "$GIT_URL" "Git $GIT_VERSION"
		printf '%s  %s\n' "$GIT_SHA256" "$archive.download" | sha256sum --check --status || fail "Git checksum mismatch"
		mv -f "$archive.download" "$archive"
	fi
	rm -rf -- "$source"
	mkdir -p "$source" "$git_prefix"
	tar -xJf "$archive" --strip-components=1 --directory "$source"
	system_cc=$(command -v cc)
	make_arguments=(
		prefix="$git_prefix"
		CC="$system_cc"
		CFLAGS="-g -O2 -Wall -std=gnu99"
		NO_EXPAT=YesPlease
		NO_GETTEXT=YesPlease
		NO_OPENSSL=YesPlease
		NO_PERL=YesPlease
		NO_TCLTK=YesPlease
	)
	(
		cd "$source"
		"$toolchain/bin/make" -j1 "${make_arguments[@]}" all
		"$toolchain/bin/make" -j1 "${make_arguments[@]}" install
	)
	printf '%s\n' "$GIT_VERSION" >"$marker"
	"$git_prefix/bin/git" --version
	"$git_prefix/bin/git" -C "$source_dir" rev-parse --verify HEAD >/dev/null
}

install_perl_modules()
{
	local archive=$tools_root/IPC-Run-${IPC_RUN_VERSION}.tar.gz
	local source=$tools_root/IPC-Run-${IPC_RUN_VERSION}
	local marker=$perl_modules/.postgamma-ipc-run-version
	local ownership_marker=${perl_modules}.postgamma-managed
	local perl5lib=$perl_modules/lib/perl5

	mkdir -p "$tools_root"
	if [[ -f "$marker" && $(cat "$marker") == "$IPC_RUN_VERSION" ]] &&
		PERL5LIB="$perl5lib" "$toolchain/bin/perl" -MIPC::Run -e 'IPC::Run->VERSION(0.94)' >/dev/null 2>&1; then
		log "reusing checksum-pinned IPC::Run $IPC_RUN_VERSION"
		printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
		return
	fi
	if [[ -e "$perl_modules" ]]; then
		[[ -f "$ownership_marker" ]] || fail "refusing to replace unmarked Perl module directory: $perl_modules"
		rm -rf -- "$perl_modules"
	fi
	printf 'managed by %s\n' "$SCRIPT_NAME" >"$ownership_marker"
	if [[ ! -f "$archive" ]] || ! printf '%s  %s\n' "$IPC_RUN_SHA256" "$archive" | sha256sum --check --status; then
		log "downloading checksum-pinned IPC::Run $IPC_RUN_VERSION"
		download_file "$archive.download" "$IPC_RUN_URL" "IPC::Run $IPC_RUN_VERSION"
		printf '%s  %s\n' "$IPC_RUN_SHA256" "$archive.download" | sha256sum --check --status || fail "IPC::Run checksum mismatch"
		mv -f "$archive.download" "$archive"
	fi
	rm -rf -- "$source"
	mkdir -p "$source" "$perl_modules"
	tar -xzf "$archive" --strip-components=1 --directory "$source"
	(
		cd "$source"
		"$toolchain/bin/perl" Makefile.PL INSTALL_BASE="$perl_modules"
		"$toolchain/bin/make" -j1
		"$toolchain/bin/make" test
		"$toolchain/bin/make" install
	)
	printf '%s\n' "$IPC_RUN_VERSION" >"$marker"
	PERL5LIB="$perl5lib" "$toolchain/bin/perl" -MIPC::Run -e 'IPC::Run->VERSION(0.94)'
}

install_compiler_wrappers()
{
	local wrapper=$toolchain/bin/postgamma-clang-driver-no-rpath
	local system_cc system_cxx system_ld
	local bridge_marker=$system_bridge/.postgamma-system-cc
	system_cc=$(command -v cc)
	system_cxx=$(command -v c++)
	system_ld=$(command -v ld)
	if [[ -e "$system_bridge" && ! -f "$bridge_marker" ]]; then
		fail "refusing to modify unmarked compiler bridge directory: $system_bridge"
	fi
	mkdir -p "$system_bridge/bin"
	printf 'managed by %s\n' "$SCRIPT_NAME" >"$bridge_marker"
	ln -sfn "$system_cc" "$system_bridge/bin/cc"
	ln -sfn "$system_cxx" "$system_bridge/bin/c++"
	ln -sfn "$system_ld" "$system_bridge/bin/ld"
	cat >"$wrapper" <<EOF
#!/bin/sh
case "\${0##*/}" in
*clangxx*) driver=$toolchain/bin/clang++ ;;
*) driver=$toolchain/bin/clang ;;
esac
link=yes
for argument do
	case "\$argument" in
	-c|-S|-E|-M|-MM|-fsyntax-only|-###|--version|-dump*|-print*) link=no ;;
	esac
done
if test "\$link" = yes; then
	exec "\$driver" --no-default-config \\
		--sysroot=$toolchain/x86_64-conda-linux-gnu/sysroot \\
		-isystem $toolchain/include \\
		-L$toolchain/lib -Wl,-rpath-link,$toolchain/lib "\$@"
fi
exec "\$driver" --no-default-config \\
	--sysroot=$toolchain/x86_64-conda-linux-gnu/sysroot \\
	-isystem $toolchain/include "\$@"
EOF
	chmod 0755 "$wrapper"
	ln -sfn "${wrapper##*/}" "$toolchain/bin/postgamma-clang-no-rpath"
	ln -sfn "${wrapper##*/}" "$toolchain/bin/postgamma-clangxx-no-rpath"
}

raise_open_file_limit()
{
	local hard_limit

	hard_limit=$(ulimit -Hn)
	if [[ "$hard_limit" != unlimited ]] && ((hard_limit < REQUIRED_NOFILE)); then
		command -v prlimit >/dev/null 2>&1 ||
			fail "prlimit is required to raise the hard open-file limit from $hard_limit to $REQUIRED_NOFILE"
		log "temporarily raising the process open-file limit from $hard_limit to $REQUIRED_NOFILE"
		run_privileged prlimit --pid "$$" \
			--nofile="$REQUIRED_NOFILE:$REQUIRED_NOFILE"
	fi
	hard_limit=$(ulimit -Hn)
	if [[ "$hard_limit" != unlimited ]] && ((hard_limit < REQUIRED_NOFILE)); then
		fail "cannot raise the hard open-file limit to $REQUIRED_NOFILE"
	fi
	ulimit -Sn "$REQUIRED_NOFILE" 2>/dev/null || fail "cannot raise the soft open-file limit to $REQUIRED_NOFILE"
}

write_runner()
{
	local runner=$work_root/run/complete-source-check.sh
	local build_dir=$work_root/build
	local strace
	strace=$(command -v strace)
	cat >"$runner" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail
runner_mode=\${1:-run}
case "\$runner_mode" in
run | preflight) ;;
*) printf 'unknown runner mode: %s\\n' "\$runner_mode" >&2; exit 2 ;;
esac
unset LANGUAGE LC_ADDRESS LC_COLLATE LC_CTYPE LC_IDENTIFICATION \
	LC_MEASUREMENT LC_MESSAGES LC_MONETARY LC_NAME LC_NUMERIC LC_PAPER \
	LC_TELEPHONE LC_TIME
export HOME=$(printf '%q' "$test_home")
export USER=$(printf '%q' "$test_user")
export LOGNAME=$(printf '%q' "$test_user")
export LC_ALL=C
export LANG=C
export PATH=$(printf '%q' "$system_bridge/bin:$git_prefix/bin:$toolchain/bin:/usr/local/bin:/usr/bin:/bin")
export LD_LIBRARY_PATH=$(printf '%q' "$toolchain/lib")
export LIBRARY_PATH=$(printf '%q' "$toolchain/lib")
export PKG_CONFIG_PATH=$(printf '%q' "$toolchain/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig")
export CPPFLAGS=$(printf '%q' "-I$toolchain/include")
export LDFLAGS=$(printf '%q' "-L$toolchain/lib")
export LLVM_CONFIG=$(printf '%q' "$toolchain/bin/llvm-config")
export CLANGXX=$(printf '%q' "$toolchain/bin/postgamma-clangxx-no-rpath")
export CC=$(printf '%q' "$toolchain/bin/postgamma-clang-no-rpath")
export CXX=$(printf '%q' "$toolchain/bin/postgamma-clangxx-no-rpath")
export AR=$(printf '%q' "$toolchain/bin/ar")
export NM=$(printf '%q' "$toolchain/bin/nm")
export READELF=$(printf '%q' "$toolchain/bin/readelf")
export OBJCOPY=$(printf '%q' "$toolchain/bin/objcopy")
export STRACE=$(printf '%q' "$strace")
export PERL=$(printf '%q' "$toolchain/bin/perl")
export PERL5LIB=$(printf '%q' "$perl_modules/lib/perl5")
export MAKE=$(printf '%q' "$toolchain/bin/make")
export PYTHON=$(printf '%q' "$toolchain/bin/python3")
export PYTHONNOUSERSITE=1
unset CONDA_PREFIX CONDA_DEFAULT_ENV MAKEFLAGS
cd $(printf '%q' "$source_dir")
"\$MAKE" source-init \\
	PYTHON="\$PYTHON" \\
	PG_REPOSITORY=$(printf '%q' "${PG_REPOSITORY:-}") \\
	PG_REPOSITORY_FALLBACK=$(printf '%q' "${PG_REPOSITORY_FALLBACK:-}") \\
	PGVECTOR_REPOSITORY=$(printf '%q' "${PGVECTOR_REPOSITORY:-}") \\
	PGVECTOR_REPOSITORY_FALLBACK=$(printf '%q' "${PGVECTOR_REPOSITORY_FALLBACK:-}")
unset PG_REPOSITORY PG_REPOSITORY_FALLBACK \\
	PGVECTOR_REPOSITORY PGVECTOR_REPOSITORY_FALLBACK
if [[ "\$runner_mode" == preflight ]]; then
	exec "\$MAKE" -j1 doctor-full-test doctor-python \\
		BUILD_DIR=$(printf '%q' "$build_dir") \\
		JOBS=$(printf '%q' "$jobs") \\
		PYTHON="\$PYTHON" MAKE="\$MAKE" CC="\$CC" CXX="\$CXX" \\
		AR="\$AR" NM="\$NM" READELF="\$READELF" OBJCOPY="\$OBJCOPY" \\
		STRACE="\$STRACE" \\
		PG_CONFIGURE_ARGS=$(printf '%q' "${POSTGAMMA_PG_CONFIGURE_ARGS:---without-icu}")
fi
exec "\$MAKE" -j1 complete-source-check \\
	BUILD_DIR=$(printf '%q' "$build_dir") \\
	JOBS=$(printf '%q' "$jobs") \\
	SESSION_EXECUTOR_TIMEOUT_SECONDS=$(printf '%q' "$session_timeout") \\
	SESSION_EXECUTOR_REQUEST_TIMEOUT_SECONDS=$(printf '%q' "$session_request_timeout") \\
	LOGICAL_MANAGEMENT_TIMEOUT_SECONDS=$(printf '%q' "$logical_management_timeout") \\
	PYTHON="\$PYTHON" MAKE="\$MAKE" CC="\$CC" CXX="\$CXX" \\
	AR="\$AR" NM="\$NM" READELF="\$READELF" OBJCOPY="\$OBJCOPY" \\
	STRACE="\$STRACE" \\
	PG_CONFIGURE_ARGS=$(printf '%q' "${POSTGAMMA_PG_CONFIGURE_ARGS:---without-icu}")
EOF
	chmod 0755 "$runner"
}

install_packages
prepare_swap
reset_managed_directory
stage_repository_inputs
prepare_source
install_toolchain
install_workflow_dependencies
install_git
install_perl_modules
install_compiler_wrappers
raise_open_file_limit
write_runner

log "verifying the provisioned environment as $test_user"
"$work_root/run/complete-source-check.sh" preflight

if [[ "$mode" == prepare ]]; then
	log "environment prepared at $work_root"
	exit 0
fi

log "starting complete-source-check with JOBS=$jobs; this can take several hours"
log "build directory: $work_root/build"
log "log: $work_root/full-test.log"
set +e
if command -v ionice >/dev/null 2>&1; then
	nice -n 10 ionice -c 2 -n 7 "$work_root/run/complete-source-check.sh" \
		2>&1 | tee "$work_root/full-test.log"
	status=${PIPESTATUS[0]}
else
	nice -n 10 "$work_root/run/complete-source-check.sh" \
		2>&1 | tee "$work_root/full-test.log"
	status=${PIPESTATUS[0]}
fi
set -e
if ((status == 0)); then
	log "complete-source-check passed"
else
	log "complete-source-check failed with exit code $status"
fi
exit "$status"
