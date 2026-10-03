#!/usr/bin/env bash

# Copyright (C) 2026-present  VMaNGOS  https://github.com/vmangos
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# macOS one-shot setup: dependencies, build, database, config, and run.
#
#   contrib/macos/setup.sh              # interactive menu
#   contrib/macos/setup.sh 1|2|3|4|5
#   contrib/macos/setup.sh deps|build|db|config|extract|run|start|stop|status
#
# Common environment variables:
#   PREFIX            install directory (default: <repo>/opt/vmangos)
#   WOW_CLIENT        WoW client root that contains Data/ (enables extraction)
#   CLIENT_BUILD      client build to emulate (default: 5875)
#   MYSQL_USER        database user (default: root)
#   MYSQL_PASS        database password (default: root)
#   MYSQL_PORT        database port (default: 3306)
#   MYSQL_ROOT_PASSWORD
#                     local root password (default: root)
#   LOGIN_PORT        realmd listen port (default: 3724)
#   WORLD_PORT        mangosd and realmlist port (default: 8085)
#   REALM_NAME        realm name (default: VMaNGOS)
#   REALM_ADDRESS     address sent to the client (default: 127.0.0.1)
#   RESET_DB=1        drop and reimport the four databases
#   FORCE_EXTRACT=1   extract client data even if maps already exist
#    
#   USEAGE:  WOW_CLIENT="/path/to/Wow" contrib/macos/setup.sh
#   with client data extraction WOW_CLIENT="/path/to/Wow" contrib/macos/setup.sh extract
#   example 
#   WOW_CLIENT="/Users/mac/Downloads/git/wow1121cn" contrib/macos/setup.sh extract

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${PREFIX:-$ROOT/opt/vmangos}"
DATA_DIR="${DATA_DIR:-$PREFIX/data}"
LOG_DIR="${LOG_DIR:-$PREFIX/logs}"
RUN_DIR="${RUN_DIR:-$PREFIX/run}"
CACHE_DIR="${CACHE_DIR:-$PREFIX/var/cache}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

CLIENT_BUILD="${CLIENT_BUILD:-5875}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-root}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root}"
LOGIN_PORT="${LOGIN_PORT:-3724}"
WORLD_PORT="${WORLD_PORT:-8085}"
REALM_NAME="${REALM_NAME:-VMaNGOS}"
REALM_ADDRESS="${REALM_ADDRESS:-127.0.0.1}"
DB_REPO="${DB_REPO:-vmangos/core}"
DB_TAG="${DB_TAG:-db_latest}"
RESET_DB="${RESET_DB:-0}"
FORCE_EXTRACT="${FORCE_EXTRACT:-0}"
DETACH="${DETACH:-0}"

JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)"

usage() {
  cat <<EOF
用法: $(basename "$0") [1-5] [选项]

  1  安装环境
  2  编译
  3  导入数据库（账号 root / root）
  4  写配置
  5  启动服务器

不带参数时会显示这个菜单并等待输入。也可以直接执行 ./setup.sh 3。

其它命令: all deps build db config extract run start stop status

选项:
  --reset   删除并重新导入数据库（也可设 RESET_DB=1）
  --detach  启动时让 mangosd 在后台运行
  -h, --help

客户端 realmlist.wtf:

  set realmlist ${REALM_ADDRESS}

世界服控制台里创建账号:

  account create admin admin
  account set gmlevel admin 6
EOF
}

show_menu() {
  cat <<EOF
请选择要执行的步骤:

  1  安装环境
  2  编译
  3  导入数据库
  4  写配置
  5  启动服务器

EOF
}

step() {
  printf '\n==> %s\n' "$1" >&2
}

validate_ports() {
  [[ "$MYSQL_PORT" =~ ^[0-9]+$ ]] || die "MYSQL_PORT 必须是数字。"
  [[ "$LOGIN_PORT" =~ ^[0-9]+$ ]] || die "LOGIN_PORT 必须是数字。"
  [[ "$WORLD_PORT" =~ ^[0-9]+$ ]] || die "WORLD_PORT 必须是数字。"
}

die() {
  printf '错误: %s\n' "$1" >&2
  exit 1
}

require_macos() {
  [[ "$(uname -s)" == "Darwin" ]] || die "这个脚本只适用于 macOS。"
}

load_brew() {
  if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
  elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
  elif command -v brew >/dev/null 2>&1; then
    eval "$(brew shellenv)"
  else
    return 1
  fi
}

brew_prefix() {
  local formula="$1"
  brew --prefix "$formula" 2>/dev/null || true
}

port_open() {
  nc -z 127.0.0.1 "$MYSQL_PORT" >/dev/null 2>&1
}

mysql_client_bin() {
  local prefix bin
  prefix="$(brew_prefix mariadb)"
  if [[ -n "$prefix" && -x "$prefix/bin/mysql" ]]; then
    printf '%s\n' "$prefix/bin/mysql"
    return
  fi
  prefix="$(brew_prefix mysql-client@8.4)"
  if [[ -n "$prefix" && -x "$prefix/bin/mysql" ]]; then
    printf '%s\n' "$prefix/bin/mysql"
    return
  fi
  prefix="$(brew_prefix mysql@8.4)"
  if [[ -n "$prefix" && -x "$prefix/bin/mysql" ]]; then
    printf '%s\n' "$prefix/bin/mysql"
    return
  fi
  bin="$(command -v mysql || true)"
  [[ -n "$bin" ]] || die "找不到 mysql 客户端。请先运行: $(basename "$0") deps"
  printf '%s\n' "$bin"
}

mysql_root() {
  local bin
  bin="$(mysql_client_bin)"
  if [[ -n "$MYSQL_ROOT_PASSWORD" ]] && "$bin" --protocol=TCP -h 127.0.0.1 -P "$MYSQL_PORT" -u root -p"$MYSQL_ROOT_PASSWORD" -e "SELECT 1" >/dev/null 2>&1; then
    "$bin" --protocol=TCP -h 127.0.0.1 -P "$MYSQL_PORT" -u root -p"$MYSQL_ROOT_PASSWORD" "$@"
    return
  fi
  if "$bin" --protocol=SOCKET -u root -e "SELECT 1" >/dev/null 2>&1; then
    "$bin" --protocol=SOCKET -u root "$@"
    return
  fi
  if "$bin" --protocol=TCP -h 127.0.0.1 -P "$MYSQL_PORT" -u root -e "SELECT 1" >/dev/null 2>&1; then
    "$bin" --protocol=TCP -h 127.0.0.1 -P "$MYSQL_PORT" -u root "$@"
    return
  fi
  "$bin" --protocol=TCP -h 127.0.0.1 -P "$MYSQL_PORT" -u root -p"$MYSQL_ROOT_PASSWORD" "$@"
}

sql_escape() {
  printf '%s' "$1" | sed "s/'/''/g"
}

wait_for_mysql() {
  local attempt
  for attempt in $(seq 1 60); do
    if mysql_root -e "SELECT 1" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "数据库在 ${MYSQL_PORT} 端口上没有在 60 秒内就绪。如果 root 需要密码，请设置 MYSQL_ROOT_PASSWORD。"
}

start_formula_server() {
  local formula="$1"
  local prefix="" server=""
  prefix="$(brew_prefix "$formula")"
  if [[ -n "$prefix" && -x "$prefix/bin/mysql.server" ]]; then
    server="$prefix/bin/mysql.server"
  fi
  step "启动 ${formula}"
  if [[ -n "$server" ]] && "$server" start; then
    return 0
  fi
  brew services start "$formula"
}

ensure_database_server() {
  if port_open; then
    step "本机 ${MYSQL_PORT} 端口已有数据库，直接使用"
    wait_for_mysql
    return
  fi

  if brew list --formula mariadb >/dev/null 2>&1; then
    start_formula_server mariadb || die "无法启动 MariaDB。"
  elif brew list --formula mysql@8.4 >/dev/null 2>&1; then
    start_formula_server mysql@8.4 || die "无法启动 MySQL 8.4。"
  elif brew list --formula mysql >/dev/null 2>&1; then
    start_formula_server mysql || die "无法启动 MySQL。"
  else
    step "安装 MariaDB"
    brew install mariadb
    start_formula_server mariadb || die "无法启动 MariaDB。"
  fi
  wait_for_mysql
}

cmd_deps() {
  step "检查编译工具"
  if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "请先装好 Xcode 命令行工具，完成后再运行本脚本。"
  fi

  if ! load_brew; then
    step "安装 Homebrew"
    NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    load_brew || die "Homebrew 已安装，但当前 shell 找不到 brew。"
  fi

  step "安装编译依赖（cmake、OpenSSL 3、MySQL 8.4 客户端）"
  brew install cmake openssl@3 mysql-client@8.4
  ensure_database_server
}

valid_client_build() {
  case "$CLIENT_BUILD" in
    5875|5464|5302|5086|4878|4695|4544|4449|4375) return 0 ;;
    *) return 1 ;;
  esac
}

cmd_build() {
  load_brew || die "找不到 Homebrew。请先运行 deps。"
  valid_client_build || die "不支持的 CLIENT_BUILD=${CLIENT_BUILD}。可用: 5875 5464 5302 5086 4878 4695 4544 4449 4375。"

  local openssl_prefix mysql_prefix
  openssl_prefix="$(brew_prefix openssl@3)"
  mysql_prefix="$(brew_prefix mysql-client@8.4)"
  [[ -n "$openssl_prefix" && -d "$openssl_prefix" ]] || die "找不到 openssl@3。请先运行 deps。"
  [[ -n "$mysql_prefix" && -d "$mysql_prefix" ]] || die "找不到 mysql-client@8.4。请先运行 deps。"

  step "配置 CMake（客户端版本 ${CLIENT_BUILD}，安装到 ${PREFIX}）"
  mkdir -p "$BUILD_DIR" "$PREFIX"
  env \
    PATH="${mysql_prefix}/bin:${PATH}" \
    MYSQL_HOME="$mysql_prefix" \
    cmake -S "$ROOT" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="${openssl_prefix};${mysql_prefix}" \
      -DBUILD_EXTRACTORS=1 \
      -DSUPPORTED_CLIENT_BUILD="$CLIENT_BUILD"

  step "编译（${JOBS} 个并行任务）"
  cmake --build "$BUILD_DIR" --parallel "$JOBS"
  cmake --install "$BUILD_DIR"
  printf '已安装到 %s\n' "$PREFIX"
}

download_world_dump() {
  local meta url name archive unpack_dir
  mkdir -p "$CACHE_DIR"
  step "查询 ${DB_REPO} 的 ${DB_TAG} 数据库快照"
  meta="$(curl --fail --location --retry 3 --retry-delay 2 --silent --show-error \
    "https://api.github.com/repos/${DB_REPO}/releases/tags/${DB_TAG}")"
  url="$(printf '%s' "$meta" | python3 -c '
import json, sys
release = json.load(sys.stdin)
assets = release.get("assets")
if not assets:
    message = release.get("message", "发布信息里没有资源")
    raise SystemExit(message)
for asset in assets:
    name = asset.get("name", "")
    if name.startswith("db-") and name.endswith(".zip") and "sqlite" not in name:
        print(asset["browser_download_url"])
        raise SystemExit(0)
raise SystemExit("没有找到 MySQL 数据库压缩包")
')"
  name="$(basename "$url")"
  archive="$CACHE_DIR/$name"
  unpack_dir="$CACHE_DIR/${name%.zip}"

  if [[ ! -f "$archive" ]]; then
    step "下载 ${name}（世界库较大，需要一些时间）"
    curl --fail --location --retry 3 --retry-delay 2 --output "$archive.partial" "$url"
    mv "$archive.partial" "$archive"
  else
    printf '使用已下载的 %s\n' "$archive" >&2
  fi

  local mangos=""
  if [[ -d "$unpack_dir" ]]; then
    mangos="$(find "$unpack_dir" -type f -name mangos.sql -print -quit)"
  fi
  if [[ -z "$mangos" ]]; then
    rm -rf "$unpack_dir"
    mkdir -p "$unpack_dir"
    unzip -q "$archive" -d "$unpack_dir"
    mangos="$(find "$unpack_dir" -type f -name mangos.sql -print -quit)"
  fi
  [[ -n "$mangos" ]] || die "压缩包里没有 mangos.sql。"
  printf '%s\n' "$(dirname "$mangos")"
}

cmd_db() {
  load_brew || die "找不到 Homebrew。请先运行 deps。"
  ensure_database_server

  local dump_dir existing
  existing="$(mysql_root -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='mangos' AND table_name='creature';")"
  if [[ "$existing" != "0" && "$RESET_DB" != "1" ]]; then
    printf 'mangos 库已有数据，跳过导入。要重来请加 --reset 或 RESET_DB=1。\n'
    return
  fi

  dump_dir="$(download_world_dump)"
  step "创建数据库，账号 ${MYSQL_USER}"
  mysql_root -e "SET GLOBAL max_allowed_packet=1073741824; SET GLOBAL sql_mode='NO_ENGINE_SUBSTITUTION';"
  # Local development uses a short password. MySQL 8 rejects it unless the
  # validate_password policy is relaxed first. Unknown variables are ignored.
  mysql_root --force -e "
SET GLOBAL validate_password.policy = 0;
SET GLOBAL validate_password.length = 4;
SET GLOBAL validate_password.mixed_case_count = 0;
SET GLOBAL validate_password.number_count = 0;
SET GLOBAL validate_password.special_char_count = 0;
SET GLOBAL validate_password.check_user_name = 0;
SET GLOBAL validate_password_policy = 0;
SET GLOBAL validate_password_length = 4;
SET GLOBAL validate_password_mixed_case_count = 0;
SET GLOBAL validate_password_number_count = 0;
SET GLOBAL validate_password_special_char_count = 0;
SET GLOBAL validate_password_check_user_name = 0;
" >/dev/null 2>&1 || true

  if [[ "$RESET_DB" == "1" ]]; then
    mysql_root -e "DROP DATABASE IF EXISTS realmd; DROP DATABASE IF EXISTS mangos; DROP DATABASE IF EXISTS characters; DROP DATABASE IF EXISTS logs;"
  fi

  mysql_root <<SQL
CREATE DATABASE IF NOT EXISTS realmd DEFAULT CHARACTER SET utf8 COLLATE utf8_general_ci;
CREATE DATABASE IF NOT EXISTS mangos DEFAULT CHARACTER SET utf8 COLLATE utf8_general_ci;
CREATE DATABASE IF NOT EXISTS characters DEFAULT CHARACTER SET utf8 COLLATE utf8_general_ci;
CREATE DATABASE IF NOT EXISTS logs DEFAULT CHARACTER SET utf8 COLLATE utf8_general_ci;
CREATE USER IF NOT EXISTS '$(sql_escape "$MYSQL_USER")'@'127.0.0.1' IDENTIFIED BY '$(sql_escape "$MYSQL_PASS")';
ALTER USER '$(sql_escape "$MYSQL_USER")'@'127.0.0.1' IDENTIFIED BY '$(sql_escape "$MYSQL_PASS")';
GRANT ALL PRIVILEGES ON *.* TO '$(sql_escape "$MYSQL_USER")'@'127.0.0.1' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL

  import_dump() {
    local database="$1"
    local file="$2"
    step "导入 ${database} <- $(basename "$file")"
    mysql_root --max-allowed-packet=1G --init-command="SET SESSION sql_mode='NO_ENGINE_SUBSTITUTION'" \
      "$database" <"$file"
  }

  [[ -f "$dump_dir/logon.sql" && -f "$dump_dir/logs.sql" && -f "$dump_dir/mangos.sql" && -f "$dump_dir/characters.sql" ]] \
    || die "数据库快照不完整: ${dump_dir}"

  import_dump realmd "$dump_dir/logon.sql"
  import_dump logs "$dump_dir/logs.sql"
  import_dump mangos "$dump_dir/mangos.sql"
  import_dump characters "$dump_dir/characters.sql"
  printf '数据库导入完成。\n'
}

patch_conf() {
  local file="$1"
  shift
  python3 - "$file" "$@" <<'PY'
import sys

path = sys.argv[1]
updates = {}
for arg in sys.argv[2:]:
    key, value = arg.split("=", 1)
    updates[key] = value

with open(path, encoding="utf-8", errors="surrogateescape") as handle:
    lines = handle.read().splitlines()

seen = set()
output = []
for line in lines:
    stripped = line.lstrip()
    replaced = False
    if stripped and not stripped.startswith("#"):
        for key, value in updates.items():
            if not stripped.startswith(key):
                continue
            rest = stripped[len(key):].lstrip()
            if rest.startswith("="):
                output.append(f"{key} = {value}")
                seen.add(key)
                replaced = True
                break
    if not replaced:
        output.append(line)

for key, value in updates.items():
    if key not in seen:
        output.append(f"{key} = {value}")

with open(path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(output) + "\n")
PY
}

db_info() {
  local database="$1"
  local pass="${MYSQL_PASS//\\/\\\\}"
  pass="${pass//\"/\\\"}"
  printf '"127.0.0.1;%s;%s;%s;%s"' "$MYSQL_PORT" "$MYSQL_USER" "$pass" "$database"
}

ensure_realm() {
  local name address
  name="$(sql_escape "$REALM_NAME")"
  address="$(sql_escape "$REALM_ADDRESS")"
  local count
  count="$(mysql_root -N -e "SELECT COUNT(*) FROM realmd.realmlist;")"
  if [[ "$count" != "0" ]]; then
    printf 'realmlist 已有 %s 条记录，保留现有服务器列表。\n' "$count"
    return
  fi
  mysql_root <<SQL
INSERT INTO realmd.realmlist
  (id, name, address, localAddress, localSubnetMask, port, icon, realmflags, timezone, allowedSecurityLevel, population, flag)
VALUES
  (1, '${name}', '${address}', '127.0.0.1', '255.255.255.0', ${WORLD_PORT}, 0, 0, 1, 0, 0, 0);
SQL
  printf '已添加服务器 %s (%s:%s)。\n' "$REALM_NAME" "$REALM_ADDRESS" "$WORLD_PORT"
}

cmd_config() {
  local realmd_dist mangosd_dist
  realmd_dist="$PREFIX/etc/realmd.conf.dist"
  mangosd_dist="$PREFIX/etc/mangosd.conf.dist"
  [[ -f "$realmd_dist" && -f "$mangosd_dist" ]] || die "找不到配置模板。请先运行 build。"
  mkdir -p "$PREFIX/etc" "$DATA_DIR" "$LOG_DIR" "$RUN_DIR"

  [[ -f "$PREFIX/etc/realmd.conf" ]] || cp "$realmd_dist" "$PREFIX/etc/realmd.conf"
  [[ -f "$PREFIX/etc/mangosd.conf" ]] || cp "$mangosd_dist" "$PREFIX/etc/mangosd.conf"

  step "写入数据库和目录配置"
  patch_conf "$PREFIX/etc/realmd.conf" \
    "LoginDatabaseInfo=$(db_info realmd)" \
    "RealmServerPort=${LOGIN_PORT}" \
    "BindIP=\"0.0.0.0\"" \
    "PidFile=\"${RUN_DIR}/realmd.pid\""

  patch_conf "$PREFIX/etc/mangosd.conf" \
    "RealmID=1" \
    "DataDir=\"${DATA_DIR}/\"" \
    "LogsDir=\"${LOG_DIR}/\"" \
    "LoginDatabase.Info=$(db_info realmd)" \
    "WorldDatabase.Info=$(db_info mangos)" \
    "CharacterDatabase.Info=$(db_info characters)" \
    "LogsDatabase.Info=$(db_info logs)" \
    "WorldServerPort=${WORLD_PORT}" \
    "BindIP=\"0.0.0.0\"" \
    "PidFile=\"${RUN_DIR}/mangosd.pid\""

  chmod 600 "$PREFIX/etc/realmd.conf" "$PREFIX/etc/mangosd.conf"
  ensure_realm
}

data_ready() {
  [[ -d "$DATA_DIR/dbc" && -d "$DATA_DIR/maps" && -d "$DATA_DIR/vmaps" && -d "$DATA_DIR/mmaps" ]]
}

cmd_extract() {
  [[ -n "${WOW_CLIENT:-}" ]] || die "请设置 WOW_CLIENT 为魔兽客户端根目录（里面要有 Data 目录）。"
  [[ -d "$WOW_CLIENT/Data" ]] || die "在 ${WOW_CLIENT} 下找不到 Data 目录。"

  local extractors="$PREFIX/bin/Extractors"
  [[ -x "$extractors/MapExtractor" ]] || die "找不到提取工具。请先用 BUILD_EXTRACTORS 编译。"
  if data_ready && [[ "$FORCE_EXTRACT" != "1" ]]; then
    printf '数据目录已有 dbc/maps/vmaps/mmaps，跳过提取。要重做请设 FORCE_EXTRACT=1。\n'
    return
  fi

  mkdir -p "$DATA_DIR/vmaps" "$DATA_DIR/mmaps"
  if [[ "$FORCE_EXTRACT" == "1" || ! -d "$DATA_DIR/dbc" || ! -d "$DATA_DIR/maps" ]]; then
    step "提取地图和 DBC"
    "$extractors/MapExtractor" --silent -i "$WOW_CLIENT" -o "$DATA_DIR"
  else
    printf 'dbc 和 maps 已存在，跳过。\n'
  fi

  if [[ "$FORCE_EXTRACT" == "1" || ! -f "$DATA_DIR/Buildings/dir_bin" ]]; then
    step "提取 vmap 原始数据"
    (
      cd "$DATA_DIR"
      "$extractors/VMapExtractor" --silent -d "$WOW_CLIENT/Data"
    )
  else
    printf 'Buildings 已存在，跳过。\n'
  fi

  if [[ "$FORCE_EXTRACT" == "1" || ! -f "$DATA_DIR/vmaps/000.vmtree" ]]; then
    step "组装 vmaps"
    mkdir -p "$DATA_DIR/vmaps"
    (
      cd "$DATA_DIR"
      "$extractors/VMapAssembler" --silent Buildings vmaps
    )
  else
    printf 'vmaps 已存在，跳过。\n'
  fi

  if [[ "$FORCE_EXTRACT" != "1" && -n "$(find "$DATA_DIR/mmaps" -name '*.mmap' -print -quit 2>/dev/null)" ]]; then
    printf 'mmaps 已存在，跳过。\n'
    printf '客户端数据已写入 %s\n' "$DATA_DIR"
    return
  fi

  step "生成 mmaps（这一步通常要数小时）"
  cp -f "$extractors/offmesh.txt" "$DATA_DIR/offmesh.txt"
  cp -f "$extractors/config.json" "$DATA_DIR/config.json"
  (
    cd "$DATA_DIR"
    "$extractors/MoveMapGenerator" --silent --threads "$JOBS" \
      --offMeshInput "$DATA_DIR/offmesh.txt" \
      --configInputPath "$DATA_DIR/config.json"
  )
  printf '客户端数据已写入 %s\n' "$DATA_DIR"
}

pid_alive() {
  local pidfile="$1"
  [[ -f "$pidfile" ]] || return 1
  local pid
  pid="$(cat "$pidfile" 2>/dev/null || true)"
  [[ -n "$pid" ]] || return 1
  kill -0 "$pid" 2>/dev/null
}

stop_pid() {
  local name="$1"
  local pidfile="$2"
  if ! pid_alive "$pidfile"; then
    rm -f "$pidfile"
    printf '%s 没有在运行。\n' "$name"
    return
  fi
  local pid
  pid="$(cat "$pidfile")"
  kill -INT "$pid" 2>/dev/null || true
  local attempt
  for attempt in $(seq 1 20); do
    if ! kill -0 "$pid" 2>/dev/null; then
      rm -f "$pidfile"
      printf '已停止 %s。\n' "$name"
      return
    fi
    sleep 1
  done
  kill -TERM "$pid" 2>/dev/null || true
  sleep 1
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  rm -f "$pidfile"
  printf '已停止 %s。\n' "$name"
}

wait_until_running() {
  local name="$1"
  local pidfile="$2"
  local launcher_pid="$3"
  local log="$4"
  local attempt
  for attempt in $(seq 1 20); do
    if pid_alive "$pidfile"; then
      sleep 1
      if pid_alive "$pidfile"; then
        return 0
      fi
      break
    fi
    if ! kill -0 "$launcher_pid" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  if kill -0 "$launcher_pid" 2>/dev/null; then
    echo "$launcher_pid" >"$pidfile"
    return 0
  fi
  tail -n 40 "$log" >&2 || true
  die "${name} 启动失败，日志在 ${log}。"
}

start_realmd() {
  if pid_alive "$RUN_DIR/realmd.pid"; then
    printf 'realmd 已在运行。\n'
    return
  fi
  mkdir -p "$LOG_DIR" "$RUN_DIR"
  rm -f "$RUN_DIR/realmd.pid"
  step "启动 realmd"
  "$PREFIX/bin/realmd" >>"$LOG_DIR/realmd.log" 2>&1 &
  wait_until_running realmd "$RUN_DIR/realmd.pid" "$!" "$LOG_DIR/realmd.log"
}

print_play_hint() {
  cat <<EOF

客户端 realmlist.wtf:
  set realmlist ${REALM_ADDRESS}

在 mangosd 控制台创建管理员账号:
  account create admin admin
  account set gmlevel admin 6

日志目录: ${LOG_DIR}
停止服务: $(basename "$0") stop
EOF
  if ! data_ready; then
    cat <<EOF

还没有地图数据。服务器可以启动，但角色进入世界会失败。
准备好 1.12 客户端后执行:
  WOW_CLIENT="/path/to/Wow" $(basename "$0") extract
EOF
  fi
}

cmd_run() {
  [[ -x "$PREFIX/bin/realmd" && -x "$PREFIX/bin/mangosd" ]] || die "找不到服务器程序。请先运行 build。"
  [[ -f "$PREFIX/etc/realmd.conf" && -f "$PREFIX/etc/mangosd.conf" ]] || die "找不到配置。请先运行 config。"
  start_realmd
  print_play_hint

  if [[ "$DETACH" == "1" || ! -t 0 || ! -t 1 ]]; then
    if pid_alive "$RUN_DIR/mangosd.pid"; then
      printf 'mangosd 已在运行。\n'
      return
    fi
    step "后台启动 mangosd"
    rm -f "$RUN_DIR/mangosd.pid"
    "$PREFIX/bin/mangosd" >>"$LOG_DIR/mangosd.log" 2>&1 &
    wait_until_running mangosd "$RUN_DIR/mangosd.pid" "$!" "$LOG_DIR/mangosd.log"
    printf 'mangosd 已在后台运行，日志: %s\n' "$LOG_DIR/mangosd.log"
    return
  fi

  if pid_alive "$RUN_DIR/mangosd.pid"; then
    printf 'mangosd 已在后台运行。先执行 stop，再重新 run 才能进入控制台。\n'
    return
  fi

  step "启动 mangosd（Ctrl-C 会同时停掉 realmd）"
  trap 'stop_pid realmd "$RUN_DIR/realmd.pid"; exit 0' INT TERM
  "$PREFIX/bin/mangosd"
  stop_pid realmd "$RUN_DIR/realmd.pid"
}

cmd_start() {
  DETACH=1
  cmd_run
}

cmd_stop() {
  stop_pid mangosd "$RUN_DIR/mangosd.pid"
  stop_pid realmd "$RUN_DIR/realmd.pid"
}

cmd_status() {
  if pid_alive "$RUN_DIR/realmd.pid"; then
    printf 'realmd  运行中  pid %s\n' "$(cat "$RUN_DIR/realmd.pid")"
  else
    printf 'realmd  未运行\n'
  fi
  if pid_alive "$RUN_DIR/mangosd.pid"; then
    printf 'mangosd 运行中  pid %s\n' "$(cat "$RUN_DIR/mangosd.pid")"
  else
    printf 'mangosd 未运行\n'
  fi
}

cmd_all() {
  cmd_deps
  cmd_build
  cmd_db
  cmd_config
  if [[ -n "${WOW_CLIENT:-}" ]]; then
    cmd_extract
  else
    printf '\n未设置 WOW_CLIENT，跳过客户端数据提取。\n'
  fi
  cmd_run
}

main() {
  require_macos
  validate_ports
  local command=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --reset) RESET_DB=1 ;;
      --detach) DETACH=1 ;;
      -h|--help) usage; exit 0 ;;
      -*) die "未知参数: $1（用 --help 查看用法）" ;;
      *)
        [[ -z "$command" ]] || die "未知参数: $1（用 --help 查看用法）"
        command="$1"
        ;;
    esac
    shift
  done
  if [[ -z "$command" ]]; then
    show_menu
    if [[ ! -t 0 ]]; then
      die "请传入 1-5。例如: $(basename "$0") 1"
    fi
    read -r -p "请输入 1-5: " command
  fi

  command="${command//[[:space:]]/}"
  case "$command" in
    1|deps) cmd_deps ;;
    2|build) cmd_build ;;
    3|db) cmd_db ;;
    4|config) cmd_config ;;
    5|run) cmd_run ;;
    all) cmd_all ;;
    extract) cmd_extract ;;
    start) cmd_start ;;
    stop) cmd_stop ;;
    status) cmd_status ;;
    help|-h|--help) usage ;;
    "") die "请输入 1-5。" ;;
    *) die "未知命令: ${command}。请输入 1-5。" ;;
  esac
}

main "$@"
