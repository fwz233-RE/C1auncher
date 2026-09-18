#!/bin/bash
# term-ime 全覆盖测试脚本

set -e

TERM_IME="./build/term-ime"
LOG_FILE="$HOME/.cache/term-ime/term-ime.log"

echo "=========================================="
echo "term-ime 全覆盖测试"
echo "=========================================="

# 检查程序存在
if [ ! -f "$TERM_IME" ]; then
    echo "❌ 错误: $TERM_IME 不存在"
    exit 1
fi

echo ""
echo "测试 1: 程序启动"
echo "--------------------"
rm -f "$LOG_FILE"
timeout 2 "$TERM_IME" < /dev/null 2>&1 || true
sleep 1
if [ -f "$LOG_FILE" ] && grep -q "term-ime starting" "$LOG_FILE"; then
    echo "✅ 程序可以启动"
else
    echo "❌ 程序启动失败"
fi

echo ""
echo "测试 2: 日志输出到文件"
echo "--------------------"
if [ -f "$LOG_FILE" ]; then
    echo "✅ 日志文件存在: $LOG_FILE"
    head -5 "$LOG_FILE" | sed 's/^/   /'
else
    echo "❌ 日志文件不存在"
fi

echo ""
echo "测试 3: Rime IME 初始化"
echo "--------------------"
if grep -q "Rime IME initialized" "$LOG_FILE" 2>/dev/null; then
    echo "✅ Rime IME 初始化成功"
elif grep -q "Failed to initialize Rime IME" "$LOG_FILE" 2>/dev/null; then
    echo "⚠️  Rime IME 初始化失败"
else
    echo "⚠️  未找到 Rime IME 初始化日志"
fi

echo ""
echo "=========================================="
echo "基础测试完成"
echo "=========================================="
