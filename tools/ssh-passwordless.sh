#!/bin/sh
#
# ssh-passwordless.sh
#
# 通过 adb 把本机 SSH 公钥安装到板子，完成 root 免密登录。
#
# 为什么不能直接用 ssh-copy-id（SDK 板子的两个坑）：
#   1. /etc/profile.d/RkEnv.sh 把登录 shell 的 HOME 导出为 /oem，
#      但 sshd 实际按 /etc/passwd 的 home (/root) 读取 authorized_keys；
#      有些工具会按 HOME=/oem 去写 /oem/.ssh，sshd 根本看不到。
#   2. 新固件里 /root 目录属主不是 root（构建环境的 uid 1000 带进来了）。
#      OpenSSH StrictModes 检查到 home 属主不对时，会拒绝所有公钥登录：
#      "Authentication refused: bad ownership or modes for directory /root"。
#      这就是 ssh-copy-id 后仍然要密码的真正原因，与密钥类型（RSA/ed25519）无关。
#
# 用法：
#   ./ssh-passwordless.sh [板子IP] [adb目标]
#   KEY=/path/to/id_rsa.pub ./ssh-passwordless.sh 192.168.1.63
#
# 默认：
#   板子 IP   192.168.1.63
#   adb 目标  192.168.1.63:5555（板端 adbd TCP 端口）
#   公钥      ~/.ssh/id_rsa.pub（不存在则回退 id_ed25519.pub）
#

set -u

SSH_HOST="${1:-192.168.1.63}"
ADB_TARGET="${ADB_TARGET:-${2:-${SSH_HOST}:5555}}"
KEY="${KEY:-}"

if [ -z "$KEY" ]; then
	if [ -f "$HOME/.ssh/id_rsa.pub" ]; then
		KEY="$HOME/.ssh/id_rsa.pub"
	elif [ -f "$HOME/.ssh/id_ed25519.pub" ]; then
		KEY="$HOME/.ssh/id_ed25519.pub"
	fi
fi

if [ -z "$KEY" ] || [ ! -f "$KEY" ]; then
	echo "error: no public key found (set KEY=/path/to/key.pub)" >&2
	exit 1
fi

PRIV="${KEY%.pub}"
if [ ! -f "$PRIV" ]; then
	echo "error: private key $PRIV not found" >&2
	exit 1
fi

echo "== board: $SSH_HOST  adb: $ADB_TARGET"
echo "== public key: $KEY"

if ! command -v adb >/dev/null 2>&1; then
	echo "error: adb not found" >&2
	exit 1
fi

if [ "$(adb -s "$ADB_TARGET" get-state 2>/dev/null)" != "device" ]; then
	echo "error: adb target $ADB_TARGET is not online" >&2
	exit 1
fi

REMOTE_KEY="/data/local/tmp/ssh_pub_$$.pub"
adb -s "$ADB_TARGET" push "$KEY" "$REMOTE_KEY" >/dev/null || {
	echo "error: failed to push public key" >&2
	exit 1
}

echo "== installing key to /root/.ssh and /oem/.ssh, fixing ownership"
adb -s "$ADB_TARGET" shell "umask 077
for d in /root /oem; do
	mkdir -p \"\$d/.ssh\"
	cat \"$REMOTE_KEY\" >> \"\$d/.ssh/authorized_keys\"
	sort -u -o \"\$d/.ssh/authorized_keys\" \"\$d/.ssh/authorized_keys\"
	chown root:root \"\$d/.ssh\" \"\$d/.ssh/authorized_keys\" 2>/dev/null
	chmod 700 \"\$d/.ssh\"
	chmod 600 \"\$d/.ssh/authorized_keys\"
done
# sshd StrictModes 要求 root home 属主为 root 且不可被组/其他写
chown root:root /root 2>/dev/null
chmod 700 /root
rm -f \"$REMOTE_KEY\"
echo installed" || {
	echo "error: failed to install key on board" >&2
	exit 1
}

echo "== verifying passwordless login"
if ssh -o BatchMode=yes -o ConnectTimeout=8 \
	-o StrictHostKeyChecking=accept-new \
	-o IdentitiesOnly=yes -i "$PRIV" \
	"root@$SSH_HOST" 'echo ssh_passwordless_ok' 2>/dev/null; then
	echo "== done: ssh root@$SSH_HOST 免密登录已生效"
	exit 0
fi

echo "error: passwordless login still failing" >&2
echo "hint: 检查板端 sshd 日志 /var/log/*，常见原因是 StrictModes 拒绝（home 属主/权限）" >&2
exit 1
