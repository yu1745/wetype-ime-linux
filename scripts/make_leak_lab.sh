#!/usr/bin/env bash
# make_leak_lab.sh <工作区目录> —— 引擎泄漏攻关的隔离实验环境。
#
# 生成:
#   <ws>/engine/   安装引擎的私有 reflink 拷贝（dicts/sysroot/qemu/lib），
#                  lib/*.so 可随意替换/打补丁，不影响其他工作区与用户安装
#   <ws>/bin/      放自行构建的 harness 二进制
#   <ws>/tmp/      容器内 TMPDIR
#   <ws>/wrun      docker 隔离运行器：本工作区 rw、当前目录(须为 git worktree)
#                  ro 挂到 /wt、主仓 NDK ro 挂到 /deps
#
# 用法（在 worktree 里）:
#   bash /home/wangyu/wetype-ime-linux/scripts/make_leak_lab.sh /tmp/wetype-xxx
#   /tmp/wetype-xxx/wrun python3 /wt/scripts/test_memory_leak.py \
#       --engine-dir /ws/engine --harness /ws/bin/jinterop --rounds 30
set -e
WS=$1
[ -n "$WS" ] || { echo "usage: $0 <workspace-dir>  (run from inside your worktree)" >&2; exit 2; }
WT=$(pwd)
[ -d "$WT/.git" ] || git -C "$WT" rev-parse --git-dir >/dev/null 2>&1 || {
    echo "must run from a git worktree of wetype-ime-linux" >&2; exit 2; }
SRC_ENGINE=$HOME/.local/lib/wetype-ime/arm64
MAIN_REPO=/home/wangyu/wetype-ime-linux
IMG=wetype-test:24.04
[ -d "$SRC_ENGINE" ] || { echo "missing installed engine: $SRC_ENGINE" >&2; exit 1; }

mkdir -p "$WS/bin" "$WS/tmp"
if [ ! -d "$WS/engine" ]; then
    echo "copying engine (reflink) ..."
    cp -r --reflink=auto "$SRC_ENGINE" "$WS/engine"
fi

# 公共镜像：ubuntu:24.04 + python3（glibc 与宿主同构，NDK host 工具链可跑）
if ! docker image inspect $IMG >/dev/null 2>&1; then
    echo "building $IMG ..."
    docker build -t $IMG - <<'EOF'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
EOF
fi

cat > "$WS/wrun" <<EOF
#!/bin/sh
# docker 隔离运行器：工作区 $WS → /ws(rw)；worktree → /wt(ro)；NDK → /deps(ro)
exec docker run --rm -i \\
  -v $WS:/ws -v $WT:/wt:ro -v $MAIN_REPO/.deps:/deps:ro \\
  -e TMPDIR=/ws/tmp -e PYTHONDONTWRITEBYTECODE=1 -e HOME=/ws/tmp \\
  -w /ws --user $(id -u):$(id -g) \\
  $IMG "\$@"
EOF
chmod +x "$WS/wrun"
echo "lab ready: $WS"
echo "  engine: $WS/engine   (patch $WS/engine/lib/*.so freely)"
echo "  runner: $WS/wrun <cmd...>   (e.g. wrun python3 /wt/scripts/test_memory_leak.py --engine-dir /ws/engine --harness /ws/bin/jinterop --rounds 30)"
