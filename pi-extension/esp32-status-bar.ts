/**
 * Pi 状态栏扩展（ESP32-C3 + SSD1315 128x32 OLED）
 *
 * 把 pi 的运行状态、上下文占用、输入输出 token、TPS 实时通过 USB 串口
 * 一行一条 JSON 推给 ESP32 状态栏，屏幕会轮播展示。
 *
 * 安装（二选一）：
 *   cp  pi-extension/esp32-status-bar.ts ~/.pi/agent/extensions/
 *   ln -s "$PWD/pi-extension/esp32-status-bar.ts" ~/.pi/agent/extensions/
 * 临时调试：pi -e ./pi-extension/esp32-status-bar.ts
 *
 * 串口选择顺序：
 *   1. 命令行 --statusbar-port /dev/ttyACM0
 *   2. 环境变量 PI_STATUSBAR_PORT=/dev/ttyACM0
 *   3. 自动探测 /dev/ttyACM* 然后 /dev/ttyUSB*
 *
 * 运行中可用 /statusbar 命令查看状态、换端口、临时关闭或发测试数据。
 */

import { closeSync, constants, openSync, readdirSync, writeSync } from "node:fs";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";

/** 最快写串口间隔，避免流式输出时把串口打满 */
const WRITE_MIN_INTERVAL_MS = 200;
/** 空闲时的保活间隔；要小于 ESP 端 CONFIG_STATUS_BAR_LINK_TIMEOUT_MS(5s) */
const KEEPALIVE_MS = 3000;
/** 串口打不开（拔掉了 / 还没插）时的重试间隔 */
const RECONNECT_MS = 5000;
/** 上下文用量的刷新间隔：getContextUsage() 会做估算，不要每帧都调 */
const CTX_REFRESH_MS = 1000;
/** 一轮结束后让 DONE 在屏幕上停留的时长 */
const DONE_HOLD_MS = 2500;

type AgentState = "idle" | "thinking" | "running" | "tool" | "waiting" | "done" | "error";

interface Snapshot {
	state: AgentState;
	ctxUsed: number;
	ctxMax: number;
	/** 本轮累计输入 token（屏幕上显示 IN） */
	tokensIn: number;
	/** 本轮累计输出 token（屏幕上显示 OUT） */
	tokensOut: number;
	tps: number;
	turn: number;
	model: string | null;
}

/* ------------------------------ 串口 ------------------------------ */

function detectPort(): string | null {
	for (const prefix of ["ttyACM", "ttyUSB"]) {
		try {
			const names = readdirSync("/dev")
				.filter((name) => name.startsWith(prefix) && /^\d+$/.test(name.slice(prefix.length)))
				.sort();
			if (names.length > 0) return `/dev/${names[0]}`;
		} catch {
			/* /dev 读不到就当作没设备 */
		}
	}
	return null;
}

class SerialLink {
	private fd: number | null = null;
	private port: string | null;
	private retryAfter = 0;
	private lastWriteAt = 0;
	private enabled = true;

	constructor(port: string | null) {
		this.port = port;
	}

	get connected(): boolean {
		return this.fd !== null;
	}

	get path(): string | null {
		return this.port;
	}

	get active(): boolean {
		return this.enabled;
	}

	usePort(port: string | null): void {
		this.close();
		this.port = port;
		this.retryAfter = 0;
	}

	setEnabled(enabled: boolean): void {
		this.enabled = enabled;
		if (!enabled) this.close();
	}

	private ensureOpen(now: number): number | null {
		if (this.fd !== null) return this.fd;
		if (now < this.retryAfter) return null;

		const port = this.port ?? detectPort();
		if (port === null) {
			this.retryAfter = now + RECONNECT_MS;
			return null;
		}

		try {
			/* O_NONBLOCK：串口写满时立刻返回 EAGAIN，绝不阻塞 pi 的主线程 */
			this.fd = openSync(port, constants.O_WRONLY | constants.O_NONBLOCK);
			this.port = port;
			return this.fd;
		} catch {
			this.fd = null;
			this.retryAfter = now + RECONNECT_MS;
			return null;
		}
	}

	/** 发送一行；force=true 时忽略节流。返回是否真的写出去了 */
	send(line: string, now: number, force = false): boolean {
		if (!this.enabled) return false;
		if (!force && now - this.lastWriteAt < WRITE_MIN_INTERVAL_MS) return false;

		const fd = this.ensureOpen(now);
		if (fd === null) return false;

		try {
			writeSync(fd, Buffer.from(`${line}\n`, "utf8"));
			this.lastWriteAt = now;
			return true;
		} catch (error) {
			const code = (error as NodeJS.ErrnoException)?.code;
			if (code === "EAGAIN" || code === "EWOULDBLOCK") {
				/* 串口缓冲满了（对端没在读），丢掉这一帧就好，连接还是好的 */
				return false;
			}
			/* 设备被拔掉或复位了：关掉，过一会儿重新枚举 */
			this.close();
			this.retryAfter = now + RECONNECT_MS;
			return false;
		}
	}

	close(): void {
		if (this.fd !== null) {
			try {
				closeSync(this.fd);
			} catch {
				/* ignore */
			}
			this.fd = null;
		}
	}
}

/* ------------------------------ 扩展主体 ------------------------------ */

export default function (pi: ExtensionAPI) {
	pi.registerFlag("statusbar-port", {
		description: "ESP32 状态栏串口设备路径（默认自动探测 /dev/ttyACM*）",
		type: "string",
	});

	const flagPort = pi.getFlag("statusbar-port");
	const initialPort = typeof flagPort === "string" && flagPort.trim() ? flagPort.trim() : null;
	const link = new SerialLink(initialPort);

	const snap: Snapshot = {
		state: "idle",
		ctxUsed: 0,
		ctxMax: 0,
		tokensIn: 0,
		tokensOut: 0,
		tps: 0,
		turn: 0,
		model: null,
	};

	let turnStartedAt = 0; // 本轮开始时间
	let runElapsed = 0; // 一轮结束后的定格耗时（秒）
	let streamStartedAt = 0; // 当前 assistant 消息开始流式输出的时间
	let streamChars = 0; // 流式收到的字符数（用于估算 token）
	let runTokensIn = 0; // 本轮累计输入 token
	let runTokensOut = 0; // 本轮累计输出 token
	let msgTokensIn = 0; // 当前 assistant 消息的输入 token
	let msgTokensOut = 0; // 当前 assistant 消息的输出 token
	let doneUntil = 0; // DONE 状态至少保持到什么时候
	let lastCtxAt = 0;
	let lastCtxUsed = 0;
	let lastCtxMax = 0;
	let timer: ReturnType<typeof setInterval> | null = null;
	let notifiedMissing = false;

	function effectiveState(now: number): AgentState {
		if (snap.state === "idle" && now < doneUntil) return "done";
		return snap.state;
	}

	function elapsedSeconds(now: number): number {
		const state = effectiveState(now);
		if (state === "idle") return 0;
		if (state === "done" || state === "error") return runElapsed;
		return turnStartedAt > 0 ? (now - turnStartedAt) / 1000 : 0;
	}

	function buildLine(now: number): string {
		const state = effectiveState(now);
		const payload: Record<string, number | string> = {
			state,
			ctx: Math.round(lastCtxUsed),
			ctx_max: Math.round(lastCtxMax),
			in: Math.round(snap.tokensIn),
			out: Math.round(snap.tokensOut),
			tps: Math.round(snap.tps * 10) / 10,
			elapsed: Math.round(elapsedSeconds(now) * 10) / 10,
			turn: snap.turn,
		};
		if (snap.model) payload.model = snap.model;
		return JSON.stringify(payload);
	}

	function refreshContextUsage(ctx: ExtensionContext | null, now: number): void {
		if (ctx === null || now - lastCtxAt < CTX_REFRESH_MS) return;
		lastCtxAt = now;
		try {
			const usage = ctx.getContextUsage();
			const window = usage?.contextWindow ?? ctx.model?.contextWindow ?? 0;
			if (window > 0) lastCtxMax = window;
			if (typeof usage?.tokens === "number" && usage.tokens > 0) {
				lastCtxUsed = usage.tokens;
			} else if (typeof usage?.percent === "number" && lastCtxMax > 0) {
				/* 有些 provider 只给百分比 */
				lastCtxUsed = Math.round((usage.percent / 100) * lastCtxMax);
			}
		} catch {
			/* 取不到就沿用上次的值 */
		}
	}

	function push(ctx: ExtensionContext | null, now: number, force = false): void {
		refreshContextUsage(ctx, now);
		link.send(buildLine(now), now, force);
	}

	function notifyOnce(ctx: ExtensionContext | null, message: string, level: "info" | "warning"): void {
		if (notifiedMissing || ctx === null) return;
		notifiedMissing = true;
		try {
			ctx.ui.notify(message, level);
		} catch {
			/* ignore */
		}
	}

	function resetRun(): void {
		snap.tokensIn = 0;
		snap.tokensOut = 0;
		snap.tps = 0;
		snap.turn = 0;
		runElapsed = 0;
		streamStartedAt = 0;
		streamChars = 0;
		runTokensIn = 0;
		runTokensOut = 0;
		msgTokensIn = 0;
		msgTokensOut = 0;
	}

	/* --------------------------- 会话生命周期 --------------------------- */

	pi.on("session_start", async (_event, ctx) => {
		snap.state = "idle";
		snap.model = ctx.model?.id ?? null;
		resetRun();
		turnStartedAt = 0;
		doneUntil = 0;
		lastCtxAt = 0;
		lastCtxUsed = 0; // 新会话，上下文占用重新算
		notifiedMissing = false;

		push(ctx, Date.now(), true);
		if (link.connected) {
			ctx.ui.notify(`状态栏已连接 ${link.path}`, "info");
		} else if (initialPort !== null) {
			/* 只在你显式指定了端口时才提醒；自动探测不到就安静待着 */
			notifyOnce(ctx, `状态栏串口打不开：${initialPort}（插上后会自动重试）`, "warning");
		}

		if (timer === null) {
			/* 空闲时也要定期发，否则 ESP 会判成 NO HOST */
			timer = setInterval(() => push(null, Date.now(), true), KEEPALIVE_MS);
		}
	});

	pi.on("session_shutdown", async () => {
		if (timer !== null) {
			clearInterval(timer);
			timer = null;
		}
		link.close();
	});

	/* ------------------------------ 运行状态 ------------------------------ */

	pi.on("agent_start", async (_event, ctx) => {
		resetRun();
		snap.state = "running";
		turnStartedAt = Date.now();
		doneUntil = 0;
		push(ctx, Date.now(), true);
	});

	pi.on("turn_start", async (_event, ctx) => {
		snap.turn += 1;
		snap.state = "thinking";
		snap.tps = 0;
		streamStartedAt = 0;
		streamChars = 0;
		msgTokensIn = 0;
		msgTokensOut = 0;
		if (turnStartedAt === 0) turnStartedAt = Date.now();
		push(ctx, Date.now(), true);
	});

	pi.on("message_update", async (event, ctx) => {
		const now = Date.now();
		const streamed = event.message;
		const usage = streamed?.role === "assistant" ? streamed.usage : undefined;
		if (usage) {
			/* 流式期间 provider 报的是当前这条消息的累计用量 */
			if (usage.input) msgTokensIn = usage.input;
			if (usage.output) msgTokensOut = usage.output;
		}

		const ev = event.assistantMessageEvent;
		if (ev) {
			switch (ev.type) {
				case "thinking_start":
				case "thinking_delta":
					if (snap.state !== "thinking") {
						snap.state = "thinking";
						push(ctx, now, true);
					}
					break;
				case "text_start":
				case "text_delta":
					if (snap.state !== "running") {
						snap.state = "running";
						push(ctx, now, true);
					}
					break;
				case "toolcall_start":
				case "toolcall_delta":
					if (snap.state !== "tool") {
						snap.state = "tool";
						push(ctx, now, true);
					}
					break;
				default:
					break;
			}

			if (ev.type === "text_delta" || ev.type === "thinking_delta" || ev.type === "toolcall_delta") {
				if (streamStartedAt === 0) streamStartedAt = now;
				streamChars += ev.delta.length;
			}
		}

		/* TPS：优先用 provider 报的 output，否则按字符数粗估（约 4 字符/token） */
		const estimatedOut = Math.max(msgTokensOut, Math.round(streamChars / 4));
		if (streamStartedAt > 0) {
			const seconds = (now - streamStartedAt) / 1000;
			if (seconds > 0.3) snap.tps = estimatedOut / seconds;
		}

		/* 累计值 = 前面已结束的消息 + 当前这条 */
		snap.tokensIn = runTokensIn + msgTokensIn;
		snap.tokensOut = runTokensOut + estimatedOut;

		push(ctx, now);
	});

	pi.on("message_end", async (event, ctx) => {
		const message = event.message;
		if (message?.role === "assistant") {
			const usage = message.usage;
			const input = usage?.input ?? msgTokensIn;
			const output = usage?.output ?? msgTokensOut;

			runTokensIn += input;
			runTokensOut += output;
			msgTokensIn = 0;
			msgTokensOut = 0;
			snap.tokensIn = runTokensIn;
			snap.tokensOut = runTokensOut;

			const seconds = streamStartedAt > 0 ? (Date.now() - streamStartedAt) / 1000 : 0;
			if (seconds > 0.3 && output > 0) snap.tps = output / seconds;

			streamStartedAt = 0;
			streamChars = 0;

			if (message.stopReason === "error" || message.errorMessage) snap.state = "error";
		}
		push(ctx, Date.now(), true);
	});

	pi.on("tool_execution_start", async (_event, ctx) => {
		snap.state = "tool";
		push(ctx, Date.now(), true);
	});

	pi.on("tool_execution_end", async (_event, ctx) => {
		if (snap.state === "tool") snap.state = "running";
		push(ctx, Date.now());
	});

	/* 等待用户输入（confirm/select/input 等弹窗） */
	pi.on("ui_prompt_start", async (_event, ctx) => {
		snap.state = "waiting";
		push(ctx, Date.now(), true);
	});

	pi.on("ui_prompt_end", async (_event, ctx) => {
		if (snap.state === "waiting") snap.state = "running";
		push(ctx, Date.now());
	});

	pi.on("agent_end", async (event, ctx) => {
		const messages = event.messages ?? [];
		const last = messages[messages.length - 1];
		const failed =
			last?.role === "assistant" &&
			(last.stopReason === "error" || Boolean(last.errorMessage));

		runElapsed = turnStartedAt > 0 ? (Date.now() - turnStartedAt) / 1000 : 0;
		snap.state = failed ? "error" : "done";
		if (!failed) doneUntil = Date.now() + DONE_HOLD_MS;
		push(ctx, Date.now(), true);
	});

	pi.on("agent_settled", async (_event, ctx) => {
		if (snap.state !== "error") snap.state = "idle";
		push(ctx, Date.now(), true);
	});

	pi.on("model_select", async (event, ctx) => {
		snap.model = event.model?.id ?? snap.model;
		if (lastCtxMax === 0 && event.model?.contextWindow) lastCtxMax = event.model.contextWindow;
		push(ctx, Date.now(), true);
	});

	/* ------------------------------ 命令 ------------------------------ */

	pi.registerCommand("statusbar", {
		description: "ESP32 状态栏：status / port <设备> / on / off / test",
		handler: async (args, ctx) => {
			const [action, value] = (args ?? "").trim().split(/\s+/);

			switch (action) {
				case "port": {
					if (!value) {
						ctx.ui.notify("用法：/statusbar port /dev/ttyACM0", "warning");
						return;
					}
					link.usePort(value);
					const ok = link.send(buildLine(Date.now()), Date.now(), true);
					ctx.ui.notify(ok ? `状态栏已切到 ${value}` : `打不开 ${value}`, ok ? "info" : "warning");
					return;
				}
				case "on": {
					link.setEnabled(true);
					push(ctx, Date.now(), true);
					ctx.ui.notify("状态栏输出已打开", "info");
					return;
				}
				case "off": {
					link.setEnabled(false);
					ctx.ui.notify("状态栏输出已关闭", "info");
					return;
				}
				case "test": {
					/* 不依赖 pi 的运行状态，直接发一段演示数据，方便验证屏幕 */
					const demo: Array<Record<string, number | string>> = [
						{ state: "idle", ctx: 8400, ctx_max: 200000, in: 0, out: 0, tps: 0, elapsed: 0, turn: 0 },
						{ state: "thinking", ctx: 9800, ctx_max: 200000, in: 1200, out: 0, tps: 0, elapsed: 1.2, turn: 1 },
						{ state: "running", ctx: 15600, ctx_max: 200000, in: 1200, out: 860, tps: 43.5, elapsed: 4.1, turn: 1 },
						{ state: "tool", ctx: 18200, ctx_max: 200000, in: 1200, out: 1520, tps: 38.2, elapsed: 8.6, turn: 2 },
						{ state: "done", ctx: 21400, ctx_max: 200000, in: 2400, out: 3100, tps: 46.8, elapsed: 15.4, turn: 3 },
					];
					for (const item of demo) {
						const ok = link.send(JSON.stringify(item), Date.now(), true);
						if (!ok) {
							ctx.ui.notify("串口不可用，请检查设备连接或 /statusbar port", "warning");
							return;
						}
						await new Promise((resolve) => setTimeout(resolve, 700));
					}
					ctx.ui.notify("已发送测试数据", "info");
					return;
				}
				default: {
					const state = link.active ? (link.connected ? "已连接" : "重试中") : "已关闭";
					ctx.ui.notify(`状态栏：${state}${link.path ? ` ${link.path}` : "（未探测到串口）"}`, "info");
					return;
				}
			}
		},
	});
}
