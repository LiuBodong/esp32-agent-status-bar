#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""向 ESP32 状态栏发送模拟 Agent 状态数据。

用法：
    uv run tools/mock_status.py                      # 自动找串口，跑一遍完整演示
    uv run tools/mock_status.py -p /dev/ttyACM0
    uv run tools/mock_status.py --mode loop          # 循环演示
    uv run tools/mock_status.py --mode idle          # 只发空闲状态
    uv run tools/mock_status.py --send '{"state":"error","tps":0}'   # 手发一条

屏幕上会实时轮播显示：状态/耗时、上下文占用、输入输出 token 与 TPS。
"""

from __future__ import annotations

import argparse
import glob
import json
import random
import sys
import threading
import time

import serial  # type: ignore


def find_port() -> str | None:
    """优先挑选 ESP32-C3 的 USB Serial/JTAG 端口（VID 303a）。"""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    for info in ports:
        if info.vid == 0x303A:
            return info.device
    if ports:
        return ports[0].device
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def send(ser: serial.Serial, payload: dict) -> None:
    line = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
    ser.write((line + "\n").encode())
    ser.flush()
    print(f"TX {line}")


def reader_loop(ser: serial.Serial, stop: threading.Event) -> None:
    """把 ESP32 发回来的 boot / hb / pong 打印出来。"""
    buf = b""
    while not stop.is_set():
        try:
            data = ser.read(256)
        except Exception:
            break
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").rstrip("\r")
            if text:
                print(f"RX {text}")


def step(ser: serial.Serial, **payload) -> None:
    send(ser, payload)


def run_demo(ser: serial.Serial, interval: float) -> None:
    """一次完整的『思考 -> 生成 -> 调用工具 -> 完成』回合。"""
    ctx_max = 200_000
    ctx = 8_400

    step(ser, state="idle", ctx=ctx, ctx_max=ctx_max, tok_in=0, tok_out=0,
         tps=0, elapsed=0, turn=0)
    time.sleep(interval)

    for turn in range(1, 4):
        ctx += random.randint(400, 1500)
        step(ser, state="thinking", ctx=ctx, ctx_max=ctx_max, turn=turn, elapsed=0)
        time.sleep(interval)

        tok_in = random.randint(600, 2400)
        tok_out = 0
        tps = random.uniform(28.0, 65.0)
        steps = 8
        for i in range(steps):
            tok_out = int(tok_in * 0.2 + (i + 1) * tps * interval)
            step(ser, state="running", ctx=ctx + tok_in + tok_out, ctx_max=ctx_max,
                 **{"in": tok_in, "out": tok_out}, tps=round(tps, 1),
                 turn=turn, elapsed=round((i + 1) * interval, 2))
            time.sleep(interval)

        ctx += tok_in + tok_out
        step(ser, state="tool", ctx=ctx, ctx_max=ctx_max,
             **{"in": tok_in, "out": tok_out}, tps=round(tps, 1),
             turn=turn, elapsed=2.4)
        time.sleep(interval)

    step(ser, state="done", ctx=ctx, ctx_max=ctx_max, tok_in=1234, tok_out=5678,
         tps=48.3, turn=3, elapsed=12.7)
    time.sleep(interval)


def run_idle(ser: serial.Serial, interval: float) -> None:
    started = time.time()
    while True:
        send(ser, {
            "state": "idle",
            "ctx": 1234,
            "ctx_max": 200000,
            "in": 0,
            "out": 0,
            "tps": 0,
            "elapsed": round(time.time() - started, 1),
        })
        time.sleep(interval)


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32 状态栏模拟数据发送器")
    parser.add_argument("-p", "--port", default=None, help="串口设备，默认自动查找")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument("-i", "--interval", type=float, default=1.0, help="每次发送间隔（秒）")
    parser.add_argument("--mode", choices=["demo", "loop", "idle"], default="demo",
                        help="demo=跑一遍完整回合; loop=循环 demo; idle=持续发空闲状态")
    parser.add_argument("--send", default=None, help="直接发送一条 JSON 后退出")
    parser.add_argument("--quiet", action="store_true", help="不打印 ESP32 回传的内容")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("找不到串口设备，请用 -p 指定，例如 -p /dev/ttyACM0", file=sys.stderr)
        return 1

    ser = serial.Serial(port, args.baud, timeout=0.2)
    print(f"已打开 {port} @ {args.baud}（Ctrl-C 退出）")

    stop = threading.Event()
    if not args.quiet:
        threading.Thread(target=reader_loop, args=(ser, stop), daemon=True).start()

    try:
        if args.send:
            send(ser, json.loads(args.send))
            time.sleep(0.5)
            return 0

        if args.mode == "idle":
            run_idle(ser, args.interval)
        elif args.mode == "loop":
            while True:
                run_demo(ser, args.interval)
        else:
            run_demo(ser, args.interval)
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        stop.set()
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
