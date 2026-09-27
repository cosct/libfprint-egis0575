#!/usr/bin/env bash
# 采集指纹数据集：驱动内置 PGM 调试路径，每 100ms 落一帧处理后的 PGM + 指标 CSV。
# 会话开始时不要碰传感器（背景预热需要空帧），然后按提示按压。
# 用法：./scripts/collect-dataset.sh <数据集名> [秒数]
# 产物：datasets/<名>-<时间戳>/{*.pgm, metrics.csv}
#   PGM 是驱动看到的最终处理帧（裁剪+背景扣除+2x+中值+stretch5 前后一致）
#   CSV 每行一帧：coverage/intensity/grain/ridge/minutiae/quality_ok 等

set -euo pipefail
cd "$(dirname "$0")/.."

# 数据集是指纹原始帧（生物特征数据）：目录/文件不给其他用户读权限
umask 077

NAME=${1:?用法: collect-dataset.sh <数据集名> [秒数]}
# 名称进入落盘路径（datasets/${NAME}-<时间戳>）：拒绝路径分隔符、..、空格等
if ! [[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
  echo "✗ 数据集名只能含字母/数字/._- 且以字母或数字开头（收到 '$NAME'）" >&2
  exit 1
fi
DUR=${2:-60}
if ! [[ "$DUR" =~ ^[0-9]+$ ]] || [[ "$DUR" -lt 1 ]]; then
  echo "✗ 秒数必须是正整数（收到 '$DUR'）" >&2
  exit 1
fi
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="datasets/${NAME}-${STAMP}"
mkdir -m 700 -p "$OUT"

if [[ ! -x ./libfprint/builddir/examples/enroll ]]; then
  echo "✗ 找不到 ./libfprint/builddir/examples/enroll（先按 README 构建）" >&2
  exit 1
fi

echo "== 采集数据集 '$NAME'（${DUR}s）→ $OUT =="
echo "  1) 前 3 秒不要碰传感器（背景预热）"
echo "  2) 之后正常按压/抬起，采集期间尽量多按几次"
echo

set +e
echo "6" | timeout -s INT -k 5 "$DUR" env G_MESSAGES_DEBUG=all \
  EGIS0575_ACTIVE_WIDTH="${EGIS0575_ACTIVE_WIDTH:-103}" \
  EGIS0575_PGM_DEBUG_DIR="$OUT" \
  EGIS0575_PGM_DEBUG_LOG="$OUT/metrics.csv" \
  EGIS0575_PGM_DEBUG_INTERVAL_MS="${EGIS0575_PGM_DEBUG_INTERVAL_MS:-100}" \
  EGIS0575_FRAME_DUMP_DIR="$OUT/raw" \
  ./libfprint/builddir/examples/enroll 2>&1 | tee "$OUT/session.log"
RUN_RC=${PIPESTATUS[1]}
set -e
if [[ "$RUN_RC" -ne 0 && "$RUN_RC" -ne 124 && "$RUN_RC" -ne 130 ]]; then
  echo "⚠ enroll 以退出码 $RUN_RC 结束（非超时/中断），下面摘要可能不完整" >&2
fi

echo
echo "== 采集结果 =="
# 不用 ls + glob：无匹配时 ls 退出非零，pipefail 下会在摘要前中止脚本
shopt -s nullglob
PGMS=("$OUT"/*.pgm)
BINS=("$OUT"/raw/*.bin)
echo "处理帧 PGM: ${#PGMS[@]} 张"
echo "原始帧 bin: ${#BINS[@]} 个"
[[ -f "$OUT/metrics.csv" ]] && head -3 "$OUT/metrics.csv"
echo
echo "下一步分析（列活跃度/死区验证）："
echo "  python3 scripts/analyze-columns.py $OUT"
