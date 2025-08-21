#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  upload_large.sh <file> [--from NAME] [--host 127.0.0.1] [--port 9080]

Description:
  - 初始化：POST /upload/init
  - 分片上传：PUT  /upload/chunk?id=...&seq=...
  - 完成提交：POST /upload/complete
  - 分片大小由服务端返回的 chunk_size（默认 256KB）

Example:
  ./upload_large.sh big.bin --from Alice --host 127.0.0.1 --port 9080
USAGE
}

# ---------- parse args ----------
FROM="Uploader"
HOST="127.0.0.1"
PORT="9080"

if [ $# -lt 1 ]; then usage; exit 1; fi
FILE="$1"; shift || true
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2;;
    --host) HOST="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1"; usage; exit 1;;
  esac
done

if [ ! -f "$FILE" ]; then
  echo "File not found: $FILE" >&2; exit 1;
fi

SIZE=$(stat -c%s "$FILE")
NAME=$(basename "$FILE")

echo "[init] file=$NAME size=$SIZE host=$HOST port=$PORT from=$FROM"

# ---------- /upload/init ----------
INIT=$(curl -sS -X POST "http://$HOST:$PORT/upload/init" \
  -H 'Content-Type: application/json' \
  -d "{\"name\":\"$NAME\",\"size\":$SIZE}")

echo "[init] resp: $INIT"
ID=$(echo "$INIT" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
CHUNK=$(echo "$INIT" | sed -n 's/.*"chunk_size":\([0-9]*\).*/\1/p')
[ -z "$ID" ] && { echo "[init] no id in response"; exit 1; }
[ -z "$CHUNK" ] && CHUNK=262144

echo "[init] id=$ID chunk_size=$CHUNK"

# ---------- upload chunks ----------
OFF=0
SEQ=0
RETRIES=3

# dd 使用字节跳过/计数（GNU dd）
while [ $OFF -lt $SIZE ]; do
  COUNT=$CHUNK; REM=$((SIZE - OFF)); [ $REM -lt $COUNT ] && COUNT=$REM

  # 读这一片
  # 注：iflag=skip_bytes,count_bytes 让 skip/count 解释为“字节”，避免块对齐问题
  CHUNK_DATA=$(mktemp)
  dd if="$FILE" of="$CHUNK_DATA" iflag=fullblock,skip_bytes,count_bytes skip=$OFF count=$COUNT status=none

  # 发送，带简单重试
  TRY=1
  while :; do
    if curl -fsS -X PUT "http://$HOST:$PORT/upload/chunk?id=$ID&seq=$SEQ" \
          --data-binary @"$CHUNK_DATA" > /dev/null; then
      break
    fi
    if [ $TRY -ge $RETRIES ]; then
      echo "[chunk] seq=$SEQ failed after $RETRIES attempts"; rm -f "$CHUNK_DATA"; exit 1
    fi
    echo "[chunk] seq=$SEQ failed, retry $TRY/$RETRIES ..."
    TRY=$((TRY+1))
    sleep 1
  done

  rm -f "$CHUNK_DATA"
  OFF=$((OFF + COUNT))
  SEQ=$((SEQ + 1))

  # 进度显示
  PCT=$((100 * OFF / SIZE))
  printf "\r[upload] %d%%  (%d / %d bytes)" "$PCT" "$OFF" "$SIZE"
done
echo

# ---------- /upload/complete ----------
COMP=$(curl -sS -X POST "http://$HOST:$PORT/upload/complete" \
  -H 'Content-Type: application/json' \
  -d "{\"id\":\"$ID\",\"name\":\"$NAME\",\"size\":$SIZE,\"from\":\"$FROM\"}")
echo "[complete] resp: $COMP"

echo "[done] upload finished. You can GET: http://$HOST:$PORT/download?name=$NAME"
