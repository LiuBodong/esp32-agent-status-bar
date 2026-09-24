#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""把 CodeBuddy Code 的会话记录实时变成 ESP32 状态栏数据。

CodeBuddy 没有 Pi 那样的进程内扩展 API（插件只能声明 skills/commands/agents/hooks），
hook 又是「一件事 spawn 一个一次性进程」，做不到 Pi 扩展那种 2 秒保活 + 100 毫秒排空
串口的常驻循环。所以这里换个法子：**常驻进程直接 tail 会话记录文件**。

数据源是 `~/.codebuddy/projects/<cwd 替换斜杠>/<session>.jsonl`。实测它是行级即时
flush 的（条目落盘延迟 4~60ms），条目类型和携带的信息：

    message(user)          新的一轮开始
    reasoning              推理结束（块完成时才落盘）
    message(assistant)     文本块结束
    function_call          工具派发；name=工具名，providerData.usage 就是这次响应的用量
    function_call_result   工具返回
    turn-metrics           一轮结束，durationMs = 整轮墙钟时间
                           带 source 字段的（background-task）不是主轮，要跳过

**粒度是「块」不是「token」**：一个块内部（推理、流式文本、工具执行）文件里完全
静默，实测最长空窗 21.7 秒。所以：

  * 拿得到的：ctx/用量/缓存命中率/模型（每次模型响应一条 usage）、工具名与工具耗时
    （function_call → function_call_result，实测精度 ~30ms）、done（turn-metrics）
  * 拿不到的：token 级进度。因此 **thinking 和 running 分不开**（两者都是一段静默），
    这里统一报 thinking；tps 只能按「一次响应」算平均，并且滞后一步。

usage 挂在「本次模型响应的最后一条条目」上：响应以工具调用收尾就挂在最后一个
function_call 上（并行工具调用时只有最后那条带），以纯文本收尾就挂在 message 上，
所以累计时要两种类型都看，且**见一条加一条**（一条 usage = 一次响应，不用去重；
providerData.conversationRequestId 是「用户轮」级别，不能拿来去重）。

用法：
    uv run tools/cb-status-daemon.py                    # 自动找串口 + 自动找会话文件
    uv run tools/cb-status-daemon.py --ctx-max 200000    # 不给的话屏幕显示 83K/--
    uv run tools/cb-status-daemon.py -t /path/to.jsonl   # 指定会话文件
    uv run tools/cb-status-daemon.py --dry-run           # 不开串口，只打印要发的帧
    uv run tools/cb-status-daemon.py --replay -t x.jsonl  # 离线快放一遍，看状态机输出
    uv run tools/cb-status-daemon.py --led-brightness 24  # 顺便把状态灯调暗（或 --led-off）

和 hook 的分工（hook 脚本见 tools/cb-status-hook.py）：
    * 没装 hook 也能跑：靠扫描项目目录里最新的 .jsonl，只是同项目开两个会话会挑错
    * 装了 hook：SessionStart 递来准确的 transcript_path，SessionEnd 通知收工 ——
      daemon 收到 end 就发 {"cmd":"bye"} 并停发（屏幕停在 NO HOST/host exit），
      收到新的 start 再恢复。reason=clear 不发 bye（紧接着就有新会话，会白闪）
    * 串口始终只有 daemon 一个进程碰，hook 只写一个小 JSON 文件

注意：串口是独占的，别和 tools/mock_status.py、Pi 扩展同时跑。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import signal
import sys
import time
from typing import Any

import serial  # type: ignore

# 轮询会话文件的间隔，同时也是排空串口的间隔
POLL_S = 0.1
# 最快写串口间隔：流式事件扎堆时别把串口打满
TX_MIN_INTERVAL_S = 0.2
# 空闲保活间隔；要明显小于 ESP 的 STATUS_BAR_LINK_TIMEOUT_MS（10 秒）
KEEPALIVE_S = 2.0
# done/error 在屏幕上停留多久再回到 idle
HOLD_S = 2.5
# 重新扫描「哪个是当前会话文件」的间隔
DISCOVER_S = 5.0
# 串口打不开/掉线后的重试间隔
RECONNECT_S = 5.0
# 算 tps 的最小响应耗时；比这还短说明是并行工具调用的兄弟条目，跳过
MIN_STEP_S = 0.3
# 明显不合理的 tps 直接丢掉（脏数据兜底）
MAX_TPS = 2000.0


def log(msg: str) -> None:
    print(msg, flush=True)


# ------------------------------ 会话文件 ------------------------------


def project_slug(cwd: str) -> str:
    """~/.codebuddy/projects 下的目录名就是 cwd 把斜杠换成横线。"""
    return cwd.strip("/").replace("/", "-")


def find_session_file(cwd: str) -> str | None:
    """找这个项目目录里最近改动的 .jsonl（多个会话同时开时会挑错，用 -t 指定即可）。"""
    home = os.path.expanduser("~")
    pattern = os.path.join(home, ".codebuddy", "projects", project_slug(cwd), "*.jsonl")
    found = glob.glob(pattern)
    if not found:
        return None
    return max(found, key=os.path.getmtime)


def session_file_path() -> str:
    """hook 写的握手文件位置；规则必须和 tools/cb-status-hook.py 里那份一致。"""
    custom = os.environ.get("CB_STATUSBAR_SESSION")
    if custom:
        return custom
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        return os.path.join(runtime, "cb-statusbar", "session.json")
    return f"/tmp/cb-statusbar-{os.getuid()}/session.json"


class SessionFile:
    """读 hook 递过来的会话握手记录（见 tools/cb-status-hook.py 头部注释）。

    start = 会话开始（带 transcript_path，比自动发现更准）
    end   = 会话结束（reason=clear 除外，那种情况紧接着就有新会话，发 bye 只会白闪）
    """

    def __init__(self, path: str) -> None:
        self.path = path
        self.mtime = 0.0
        self.record: dict | None = None

    def read(self) -> dict | None:
        try:
            st = os.stat(self.path)
        except OSError:
            self.mtime = 0.0
            self.record = None
            return None
        if st.st_mtime != self.mtime:
            self.mtime = st.st_mtime
            try:
                with open(self.path, encoding="utf-8") as fh:
                    self.record = json.load(fh)
            except (OSError, ValueError):
                self.record = None
        return self.record


class TranscriptTailer:
    """增量读会话文件：按字节偏移读，处理换行粘包、截断和换文件。"""

    def __init__(self, explicit: str | None, cwd: str) -> None:
        self.explicit = explicit
        self.cwd = cwd
        self.preferred: str | None = None  # hook 递过来的路径，优先于自动发现
        self.path: str | None = None
        self.fh: Any = None
        self.ino: int | None = None
        self.buf = ""
        self.next_discover = 0.0
        self.newest: str | None = None

    def _close(self) -> None:
        if self.fh is not None:
            try:
                self.fh.close()
            except OSError:
                pass
        self.fh = None
        self.ino = None
        self.buf = ""

    def _resolve(self, now: float) -> str | None:
        """优先级：命令行指定 > hook 递来的 > 目录里最新的。"""
        if self.explicit:
            return self.explicit if os.path.exists(self.explicit) else None
        if self.preferred and os.path.exists(self.preferred):
            return self.preferred
        if now >= self.next_discover:
            self.next_discover = now + DISCOVER_S
            self.newest = find_session_file(self.cwd)
        return self.newest

    def poll(self) -> tuple[list[dict], bool]:
        """返回 (新条目, 是否换了文件)。换文件表示要重置状态机。"""
        now = time.monotonic()
        want = self._resolve(now)
        restarted = False

        if want is None:
            return [], False

        if self.fh is not None and want != self.path:
            # 会话目录里冒出了更新的文件（新会话 / /clear）
            log(f"[tail] 切到会话文件 {want}")
            self._close()
            restarted = True

        if self.fh is None:
            try:
                self.fh = open(want, "r", encoding="utf-8", errors="replace")
                self.ino = os.fstat(self.fh.fileno()).st_ino
            except OSError:
                self.fh = None
                return [], restarted
            self.path = want
            if not restarted:
                log(f"[tail] 跟踪 {want}")

        # 文件被替换（inode 变了）或截断时，从头再来
        try:
            st = os.stat(want)
        except OSError:
            self._close()
            return [], True
        if self.ino is not None and st.st_ino != self.ino:
            self._close()
            return self.poll()

        entries: list[dict] = []
        try:
            data = self.fh.read()
        except (OSError, ValueError):
            self._close()
            return [], True
        if not data:
            return [], restarted

        self.buf += data
        lines = self.buf.split("\n")
        self.buf = lines.pop()  # 最后一段可能是半行，留到下次
        for raw in lines:
            raw = raw.strip()
            if not raw.startswith("{"):
                continue  # 非 JSON 行（理论上不会有）
            try:
                entries.append(json.loads(raw))
            except ValueError:
                continue  # 半行/坏行直接丢，不打断后面
        if len(self.buf) > 1 << 20:
            self.buf = ""

        return entries, restarted


# ------------------------------ 状态机 ------------------------------


class AgentTracker:
    """把条目序列翻译成屏幕要的那几个字段。

    条目本身就构成一台状态机：
        新一轮        message(user)                        -> thinking
        推理/输出中   （静默期，没有条目）                  -> thinking
        工具运行      function_call .. function_call_result -> tool
        一轮结束      turn-metrics（不带 source）           -> done 保持 2.5 秒 -> idle
    """

    def __init__(self, ctx_max: int) -> None:
        self.ctx_max = ctx_max
        self.reset()

    def reset(self) -> None:
        self.state = "idle"
        self.model: str | None = None
        self.ctx_used = 0
        self.cache_pct = -1.0
        self.tok_in = 0
        self.tok_out = 0
        self.tps = 0.0
        self.call_id: str | None = None  # 最近派发的工具调用 id
        self.tool_open = False  # 这个调用还没等到结果（= 工具在跑）
        self.boundary_ts = 0.0  # 本轮响应的起点（上一条工具结果 / 用户消息）
        self.hold_until = 0.0
        self.hinted_ctx = False

    def state_at(self, now: float) -> str:
        if self.state in ("done", "error") and now >= self.hold_until:
            return "idle"
        return self.state

    def _end_tool_phase(self) -> None:
        """模型开始新的响应了（reasoning/文本/新一轮/轮尾），说明工具阶段已经结束。

        不能只靠 function_call_result 配对：实测会话里有被中断的调用（function_call
        带 status=incomplete，或者干脆没有结果条目），只认结果的话状态会永久卡在 tool。
        """
        self.tool_open = False
        if self.state == "tool":
            self.state = "thinking"

    def feed(self, e: dict, now: float) -> None:
        ts = (e.get("timestamp") or 0) / 1000.0
        etype = e.get("type")
        pd = e.get("providerData") or {}
        if pd.get("model"):
            self.model = pd["model"]

        # 一次模型响应一条 usage：见一条加一条，不用去重（见文件头注释）
        usage = pd.get("usage")
        if usage:
            inn = usage.get("inputTokens") or 0
            out = usage.get("outputTokens") or 0
            details = usage.get("inputTokensDetails") or [{}]
            cached = (details[0] or {}).get("cached_tokens") or 0
            self.tok_in += inn
            self.tok_out += out
            self.ctx_used = inn
            self.cache_pct = round(100.0 * cached / inn, 1) if inn > 0 else -1.0
            # tps 只能按「一次响应」算平均：out / 本次响应耗时（滞后一步才知道）
            step = ts - self.boundary_ts
            if step >= MIN_STEP_S and out > 0:
                tps = out / step
                if tps <= MAX_TPS:
                    self.tps = tps
            if self.ctx_max <= 0 and self.model and not self.hinted_ctx:
                self.hinted_ctx = True
                log(
                    f"[ctx] 模型 {self.model} 的上下文窗口未知，用 --ctx-max 指定（否则屏幕显示 83K/--）"
                )

        if etype == "message":
            if e.get("role") == "user":
                self.state = "thinking"
                self.tps = 0.0
                self.boundary_ts = ts
            self._end_tool_phase()
            return

        if etype == "function_call":
            if e.get("status") not in (None, "completed"):
                # 被中断的调用（status=incomplete）不会有结果条目，开了就永远卡在 tool
                self._end_tool_phase()
                return
            # 并行调用会连着来几条，只有最后一条扛 usage；用最后一条的 id 判工具阶段结束
            self.call_id = e.get("callId")
            self.tool_open = True
            self.state = "tool"
            return

        if etype == "function_call_result":
            if e.get("callId") == self.call_id:
                self.tool_open = False
            self.boundary_ts = ts
            if e.get("status") not in (None, "completed"):
                self.state = "error"  # incomplete = 被中断或失败
                self.hold_until = now + HOLD_S
            elif not self.tool_open:
                self.state = "thinking"
            return

        if etype == "turn-metrics":
            self._end_tool_phase()
            if e.get("source"):
                return  # background-task 之类的旁支轮次，不是主轮结束
            self.state = "done"
            self.hold_until = now + HOLD_S
            self.boundary_ts = ts
            return

        if etype in ("reasoning", "summary"):
            self._end_tool_phase()

    def frame(self, now: float) -> dict:
        """组装一行给 ESP 的 JSON。

        刻意不发 elapsed 和 ctx_pct：
          * elapsed 由 ESP 自己从「状态变化时刻」计时，实测比每帧下发更顺（帧间隔 2 秒会跳）；
          * ctx_pct 让 ESP 用 ctx/ctx_max 自己算（它的百分比逻辑就是为这条兜底路径写的）。
        """
        payload: dict[str, Any] = {
            "state": self.state_at(now),
            "ctx": self.ctx_used,
            "ctx_max": self.ctx_max,
            "cache_pct": self.cache_pct,
            "in": self.tok_in,
            "out": self.tok_out,
            "tps": round(self.tps, 1),
        }
        if self.model:
            payload["model"] = self.model
        return payload


# ------------------------------ 串口 ------------------------------


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


class SerialLink:
    """双工串口：写状态行，同时把 ESP 的下行读干净。

    必须读：ESP 每 10 秒发心跳，只写不读会把 host 的 tty 输入队列（4KB）涨满，
    反向把 ESP 的 TX 环堵死，最后连自己的写也失败（表现是「连着却闪 NO HOST」）。
    pyserial 打开时会把 tty 设成 raw（关掉 icanon/echo/ixon），这一步不能省：
    ECHO 会把 ESP 自己的下行回灌进它的 RX，和真数据粘成半行。
    """

    def __init__(self, port: str, baud: int, dry_run: bool) -> None:
        self.port = port
        self.baud = baud
        self.dry_run = dry_run
        self.ser: Any = None
        self.retry_after = 0.0
        self.rx_buf = b""
        self.hb_count = 0
        self.esp: dict = {}

    def ensure(self, now: float) -> None:
        """打开串口；失败或掉线就过一会儿重试（ESP 复位、拔插都会走到这里）。"""
        if self.ser is not None or self.dry_run:
            return
        if now < self.retry_after:
            return
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0, write_timeout=0.2)
            log(f"[link] 已打开 {self.port} @ {self.baud}")
        except Exception as exc:
            self.ser = None
            self.retry_after = now + RECONNECT_S
            log(f"[link] 打不开 {self.port}（{exc}），{RECONNECT_S:g} 秒后重试")

    def drain(self) -> None:
        """读下行并解析 boot / hb，用来判断链路是否真的健康（rx/bad 单调涨 = 有问题）。"""
        if self.ser is None:
            return
        try:
            data = self.ser.read(4096)
        except Exception:  # 设备被拔掉/复位
            self.ser = None
            self.retry_after = time.monotonic() + RECONNECT_S
            return
        if not data:
            return
        self.rx_buf += data
        while b"\n" in self.rx_buf:
            raw, self.rx_buf = self.rx_buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").rstrip("\r")
            if not text.startswith("{"):
                continue
            try:
                evt = json.loads(text)
            except ValueError:
                continue
            if evt.get("evt") == "hb":
                self.hb_count += 1
                self.esp = evt
            elif evt.get("evt") == "boot":
                self.hb_count = 0
                self.esp = evt
                log(f"[esp] 收到 boot：{text}")
        if len(self.rx_buf) > 4096:
            self.rx_buf = b""

    def send(self, payload: dict) -> bool:
        line = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
        if self.dry_run:
            return True
        if self.ser is None:
            return False
        try:
            self.ser.write((line + "\n").encode())
        except Exception:
            self.ser = None
            self.retry_after = time.monotonic() + RECONNECT_S
            return False
        return True

    def close(self, say_bye: bool) -> None:
        if self.ser is not None:
            if say_bye:
                # 告诉 ESP 是主机主动退出，屏幕立刻显示 host exit，不用等 10 秒超时
                try:
                    self.ser.write(b'{"cmd":"bye"}\n')
                    self.ser.flush()
                except Exception:
                    pass
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None


# ------------------------------ 主循环 ------------------------------


def replay(path: str, ctx_max: int, limit: int | None) -> int:
    """离线快放一遍会话文件，把每个条目的推导结果打出来，不开串口。"""
    tracker = AgentTracker(ctx_max)
    n = 0
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    for line in lines:
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            e = json.loads(line)
        except ValueError:
            continue
        n += 1
        if limit and n > limit:
            break
        ts = (e.get("timestamp") or 0) / 1000.0
        tracker.feed(e, ts)
        f = tracker.frame(ts)
        tool = ""
        if e.get("type") == "function_call":
            tool = f" tool={e.get('name')}"
        elif e.get("type") == "function_call_result":
            tool = f" status={e.get('status')}"
        elif e.get("type") == "message":
            tool = f" role={e.get('role')}"
        elif e.get("type") == "turn-metrics":
            tool = f" dur={e.get('durationMs')}ms src={e.get('source')}"
        log(
            f"{n:<5d} {e.get('type') or ''!s:<20s}{tool:<42s} state={f['state']:<8s}"
            f" ctx={f['ctx']:<7d} in={f['in']:<7d} out={f['out']:<7d}"
            f" cache={f['cache_pct']:<5} tps={f['tps']:.1f}"
        )
    log(
        f"--- 共 {n} 条，最终帧: {json.dumps(tracker.frame(time.time()), ensure_ascii=True)}"
    )
    return 0


def run(args: argparse.Namespace) -> int:
    cwd = args.cwd or os.getcwd()
    ctx_max = args.ctx_max or int(os.environ.get("CB_STATUSBAR_CTX_MAX", "0") or 0)
    tracker = AgentTracker(ctx_max)
    tailer = TranscriptTailer(args.transcript, cwd)
    session = SessionFile(args.session_file or session_file_path())

    port = args.port
    if not args.dry_run:
        port = port or find_port()
        if not port:
            print("找不到串口设备，请用 -p 指定，例如 -p /dev/ttyACM0", file=sys.stderr)
            return 1

    link = SerialLink(port or "(dry-run)", args.baud, args.dry_run)
    link.ensure(time.monotonic())
    if ctx_max:
        log(f"[ctx] 上下文窗口按 {ctx_max} 算")

    stop = {"flag": False}

    def on_signal(_sig: int, _frm: Any) -> None:
        stop["flag"] = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    last_tx = 0.0
    last_sent: dict | None = None
    paused = False
    seen_rec: dict | None = None

    # 状态灯参数：串口（重）连上之后下发一次。ESP 重启会回到 Kconfig 默认值，
    # 所以断线重连后要再发一遍，而不是只在进程启动时发一次。
    led_cmd: dict[str, Any] | None = None
    if args.led_off:
        led_cmd = {"cmd": "led", "on": False}
    elif args.led_brightness is not None:
        led_cmd = {"cmd": "led", "brightness": max(0, min(255, args.led_brightness))}
    led_applied = False

    while not stop["flag"]:
        now_mono = time.monotonic()
        now = time.time()

        # hook 的握手文件：给的是「当前会话是谁」和「会话结没结束」
        rec = session.read()
        if rec is not None and rec != seen_rec:
            first = seen_rec is None
            seen_rec = rec
            if rec.get("event") == "start":
                tailer.preferred = rec.get("transcript_path")
                if paused:
                    paused = False
                    last_sent = None
                    log("[hook] 新会话开始，恢复推送")
            elif rec.get("event") == "end":
                reason = rec.get("reason")
                if first:
                    # 启动时看到的是历史状态（上次会话已经结束了），不补发 bye
                    paused = True
                elif reason == "clear":
                    # /clear 紧接着就有 SessionStart，发 bye 只会让屏幕白闪一下
                    pass
                elif not paused:
                    link.send({"cmd": "bye"})
                    paused = True
                    last_sent = None
                    log(f"[hook] 会话结束（{reason}），已发 bye")

        entries, restarted = tailer.poll()
        if restarted:
            tracker.reset()
            last_sent = None
        for e in entries:
            tracker.feed(e, now)

        link.ensure(now_mono)
        link.drain()

        if led_cmd is not None:
            if args.dry_run:
                if not led_applied:
                    led_applied = True
                    log(
                        f"[led] dry-run，不占串口：{json.dumps(led_cmd, ensure_ascii=True)}"
                    )
            elif link.ser is None:
                led_applied = False  # 掉线了，等重连上再补发
            elif not led_applied and link.send(led_cmd):
                led_applied = True
                log(f"[led] 已下发 {json.dumps(led_cmd, ensure_ascii=True)}")

        frame = tracker.frame(now)
        # 状态或数字变了就发；没变也要按 KEEPALIVE_S 发一次，否则 ESP 判 NO HOST。
        # paused = hook 说了会话已结束，这时候必须闭嘴，让屏幕停在 NO HOST/host exit
        stale = last_sent is None or frame != last_sent
        due = now_mono - last_tx >= KEEPALIVE_S
        if (
            not paused
            and (stale or due)
            and now_mono - last_tx >= TX_MIN_INTERVAL_S
            and link.send(frame)
        ):
            last_tx = now_mono
            last_sent = frame
            if not args.quiet:
                esp = ""
                if link.esp:
                    esp = f"  [esp hb={link.hb_count} rx={link.esp.get('rx')} bad={link.esp.get('bad')}]"
                log(
                    f"{time.strftime('%H:%M:%S')} {json.dumps(frame, ensure_ascii=True)}{esp}"
                )

        time.sleep(POLL_S)

    link.close(say_bye=True)
    log("\n已停止")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CodeBuddy 会话 -> ESP32 状态栏（tail 会话记录）"
    )
    parser.add_argument("-p", "--port", default=None, help="串口设备，默认自动查找")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument(
        "-t", "--transcript", default=None, help="会话 .jsonl 路径，默认自动找最新的"
    )
    parser.add_argument(
        "--session-file",
        default=None,
        help="hook 写的握手文件，默认 $CB_STATUSBAR_SESSION 或 $XDG_RUNTIME_DIR/cb-statusbar/session.json",
    )
    parser.add_argument(
        "--cwd", default=None, help="按哪个目录找会话文件（默认当前目录）"
    )
    parser.add_argument(
        "--ctx-max",
        type=int,
        default=0,
        help="上下文窗口大小；不给则屏幕显示 83K/--（transcript 里没有这个值）",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="不开串口，只打印要发的帧"
    )
    parser.add_argument(
        "--replay", action="store_true", help="离线快放 --transcript，验证状态机"
    )
    parser.add_argument(
        "--limit", type=int, default=0, help="--replay 时最多处理多少条"
    )
    parser.add_argument(
        "--led-brightness",
        type=int,
        default=None,
        help="启动后把状态灯亮度设成 0..255（不落盘，ESP 重启回 Kconfig 默认）",
    )
    parser.add_argument(
        "--led-off", action="store_true", help="启动后关掉状态灯（串口乱入的临时开关）"
    )
    parser.add_argument("--quiet", action="store_true", help="不打印每帧")
    args = parser.parse_args()

    if args.replay:
        path = args.transcript or find_session_file(args.cwd or os.getcwd())
        if not path:
            print("没找到会话文件，请用 -t 指定 .jsonl", file=sys.stderr)
            return 1
        log(f"[replay] {path}")
        return replay(path, args.ctx_max, args.limit or None)

    return run(args)


if __name__ == "__main__":
    sys.exit(main())
