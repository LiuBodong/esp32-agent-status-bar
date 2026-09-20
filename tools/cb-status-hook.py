#!/usr/bin/env python3
"""CodeBuddy Code hook：把「当前会话是谁」告诉 cb-status-daemon。

为什么需要它：daemon 默认靠扫描 ~/.codebuddy/projects/<项目>/*.jsonl 猜当前会话，
同一个项目开两个会话就会挑错文件；而且 agent 退出时它无从得知，没法立刻发 bye。
这个 hook 补上这两件事——SessionStart 递 transcript_path，SessionEnd 通知收工。

它只做一件事：往一个小的握手文件里写一条记录（原子替换），不碰串口。
串口由 daemon 独占，hook 绝不能自己去开——两个进程抢同一个 tty 会把 raw 设置
和字节流都搅乱。

握手文件（daemon 侧同名逻辑见 tools/cb-status-daemon.py:session_file_path）：
    $CB_STATUSBAR_SESSION，否则 $XDG_RUNTIME_DIR/cb-statusbar/session.json，
    否则 /tmp/cb-statusbar-<uid>/session.json
    内容：{"event":"start"|"end", ...}

注意：**绝不能往 stdout 写东西**。SessionStart 的 stdout 会被灌进模型上下文，
打印个 "ok" 都会污染对话。所以这里只写文件，什么都不打印，出错也静默退 0——
hook 挂掉不该影响会话。

安装：
    mkdir -p ~/.codebuddy/hooks
    cp tools/cb-status-hook.py ~/.codebuddy/hooks/
然后在 ~/.codebuddy/settings.json 里挂上（片段见 AGENTS.md）。
"""

import json
import os
import sys
import time


def session_path() -> str:
    """和 daemon 里那份规则保持一致。"""
    custom = os.environ.get("CB_STATUSBAR_SESSION")
    if custom:
        return custom
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        base = os.path.join(runtime, "cb-statusbar")
    else:
        base = f"/tmp/cb-statusbar-{os.getuid()}"
    return os.path.join(base, "session.json")


def build(payload: dict) -> dict | None:
    event = payload.get("hook_event_name")
    base = {
        "session_id": payload.get("session_id"),
        "cwd": payload.get("cwd"),
        "ts": time.time(),
    }
    if event == "SessionStart":
        base["event"] = "start"
        base["transcript_path"] = payload.get("transcript_path")
        base["source"] = payload.get("source")
        return base
    if event == "SessionEnd":
        base["event"] = "end"
        base["reason"] = payload.get("reason")
        return base
    return None  # 别的钩子事件不管


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(payload, dict):
        return 0

    record = build(payload)
    if record is None:
        return 0
    if record["event"] == "start" and not record.get("transcript_path"):
        # start 没带路径就没意义，daemon 会退回自动发现
        return 0

    path = session_path()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(record, fh, ensure_ascii=False)
        os.replace(tmp, path)  # 原子替换：daemon 不会读到半截 JSON
    except OSError:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
