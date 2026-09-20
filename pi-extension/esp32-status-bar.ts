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
 * 串口必须是「双工 + raw」：
 *   - ESP 每 10s 会下行心跳，boot/pong/ack 也都是下行。如果只写不读，这些数据会把
 *     host 的 tty 输入队列（canonical 上限 4KB）涨满，USB IN 方向就没人消费了；
 *     ESP 的 TX 环（512B）跟着堵死，而它的心跳发送又在 RX 任务里，连 RX 一起拖死 ——
 *     表现出来就是 agent 明明连着，屏幕却偶现 NO HOST。所以这里持续排空。
 *   - tty 默认是 icanon/echo/ixon，ECHO 会把 ESP 自己的输出回灌进它自己的 RX，
 *     和主机的真数据按字节交错粘成半行，ESP 解析失败并把回显当成主机存活。
 *     所以打开后立刻 stty raw（Node 没有 tcsetattr，只能借系统命令）。
 *
 * 运行中可用 /statusbar 命令查看状态、换端口、临时关闭或发测试数据。
 */

import { spawnSync } from "node:child_process";
import { closeSync, constants, openSync, readSync, readdirSync, writeSync } from "node:fs";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";

/** 最快写串口间隔，避免流式输出时把串口打满 */
const WRITE_MIN_INTERVAL_MS = 200;
/** 排空串口的间隔；不排空会反向把 ESP 的 RX 拖死（见文件头注释） */
const DRAIN_MS = 100;
/** 空闲时的保活间隔；要明显小于 ESP 端 CONFIG_STATUS_BAR_LINK_TIMEOUT_MS */
const KEEPALIVE_MS = 2000;
/** 待发送缓冲上限：超了说明对端长时间收不动，整块丢弃重来，免得无限膨胀 */
const PENDING_MAX = 4096;
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

/** ESP 侧心跳里带上来的自检数据，用来在 /statusbar 里判断链路是否真的健康 */
interface EspStats {
	/** 收到心跳包的数量 */
	hbs: number;
	/** 最近一次心跳的 ESP 运行时长（秒） */
	upSec: number;
	/** ESP 累计解析成功 / 失败的主机行数 */
	rx: number;
	bad: number;
}

class SerialLink {
	private fd: number | null = null;
	private port: string | null;
	private retryAfter = 0;
	private lastWriteAt = 0;
	private enabled = true;

	/** 待发送的字节；写不进去时留着下次补，保证不把 JSON 行截成半行 */
	private pending: Buffer = Buffer.alloc(0);
	/** 下行拆行用的残留 */
	private rxTail = "";
	private readonly rxChunk = Buffer.alloc(4096);
	private rawModeWarned = false;
	private lastRxAt = 0;
	private stats: EspStats | null = null;

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

	/** ESP 最近一次下行的时间（0 = 从没收到过） */
	get lastRxMs(): number {
		return this.lastRxAt;
	}

	get espStats(): EspStats | null {
		return this.stats;
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
			/* O_RDWR：必须能读，ESP 的下行不排空会把链路堵死（见文件头注释）。
			 * O_NONBLOCK：串口写满时立刻返回 EAGAIN，绝不阻塞 pi 的主线程 */
			this.fd = openSync(port, constants.O_RDWR | constants.O_NONBLOCK);
			this.port = port;
			this.rxTail = "";
			this.pending = Buffer.alloc(0);
			this.applyRawMode(port);
			return this.fd;
		} catch {
			this.fd = null;
			this.retryAfter = now + RECONNECT_MS;
			return null;
		}
	}

	/** 关掉 tty 的 icanon/echo/ixon，见文件头注释；没有 stty（如 Windows）就跳过 */
	private applyRawMode(port: string): void {
		const args = process.platform === "darwin" ? ["-f", port, "raw"] : ["-F", port, "raw"];
		const result = spawnSync("stty", args, { stdio: "ignore" });
		if ((result.error || result.status !== 0) && !this.rawModeWarned) {
			this.rawModeWarned = true;
			/* 没有 stty 也还能用：ESP 侧只把「带主机字段的行」算作主机存活，
			 * 不会把回显当成主机。这里只提醒一声，不打断发送。 */
			console.error(`[statusbar] stty raw ${port} 失败，串口回显可能污染协议流`);
		}
	}

	/**
	 * 把 ESP 的下行读干净。除了防止 tty 缓冲涨满，顺便解析 boot/hb/pong/ack，
	 * 这样 /statusbar 能看到对端是否真的活着。
	 */
	drain(now = Date.now()): void {
		const fd = this.fd;
		if (fd === null) return;

		for (;;) {
			let n: number;
			try {
				n = readSync(fd, this.rxChunk, 0, this.rxChunk.length, null);
			} catch (error) {
				const code = (error as NodeJS.ErrnoException)?.code;
				if (code === "EAGAIN" || code === "EWOULDBLOCK" || code === "EINTR") break;
				this.close();
				this.retryAfter = now + RECONNECT_MS;
				return;
			}
			if (n <= 0) break;
			this.consume(this.rxChunk.subarray(0, n), now);
		}

		this.flush(now);
	}

	private consume(chunk: Buffer, now: number): void {
		this.lastRxAt = now;
		const text = this.rxTail + chunk.toString("utf8");
		const lines = text.split("\n");
		this.rxTail = lines.pop() ?? ""; // 最后一段可能是半行，留到下次

		for (const raw of lines) {
			const line = raw.endsWith("\r") ? raw.slice(0, -1) : raw;
			if (!line.startsWith("{")) continue; // ESP 日志/别的杂音
			try {
				const evt = JSON.parse(line) as Record<string, unknown>;
				if (typeof evt.evt !== "string") continue;
				if (evt.evt === "hb") {
					this.stats = {
						hbs: (this.stats?.hbs ?? 0) + 1,
						upSec: typeof evt.up_ms === "number" ? Math.round(evt.up_ms / 1000) : 0,
						rx: typeof evt.rx === "number" ? evt.rx : 0,
						bad: typeof evt.bad === "number" ? evt.bad : 0,
					};
				} else if (evt.evt === "boot") {
					/* 上电/复位后 ESP 会主动报一声，顺带把统计清零 */
					this.stats = { hbs: 0, upSec: 0, rx: 0, bad: 0 };
				}
			} catch {
				/* 下行杂音，忽略 */
			}
		}
		/* rxTail 无限增长只在协议对端乱发无换行数据时发生，兜一下 */
		if (this.rxTail.length > 4096) this.rxTail = "";
	}

	/** 尽量把 pending 发出去；发不完的留给下一轮（drain 定时器会续） */
	private flush(now: number): void {
		if (this.pending.length === 0) return;
		const fd = this.fd;
		if (fd === null) return;

		try {
			const n = writeSync(fd, this.pending);
			if (n <= 0) return; // 串口写满：剩下的留给下一轮
			this.pending = this.pending.subarray(n);
			if (this.pending.length === 0) this.lastWriteAt = now;
		} catch (error) {
			const code = (error as NodeJS.ErrnoException)?.code;
			if (code === "EAGAIN" || code === "EWOULDBLOCK") {
				/* 串口缓冲满了（对端没在读），这一轮发不动而已，连接还是好的 */
				return;
			}
			/* 设备被拔掉或复位了：关掉，过一会儿重新枚举 */
			this.close();
			this.retryAfter = now + RECONNECT_MS;
		}
	}

	/**
	 * 发送一行；force=true 时忽略节流。
	 * @returns 这一行是否已经交给串口（可能还在 pending 里等下一轮补发）
	 */
	send(line: string, now: number, force = false): boolean {
		if (!this.enabled) return false;
		if (!force && now - this.lastWriteAt < WRITE_MIN_INTERVAL_MS) return false;

		const fd = this.ensureOpen(now);
		if (fd === null) return false;

		const frame = Buffer.from(`${line}\n`, "utf8");
		if (this.pending.length + frame.length > PENDING_MAX) {
			/* 对端长时间收不动：整块丢掉重来，宁可丢几帧也不能攒出半行粘在别人后面 */
			this.pending = Buffer.alloc(0);
		}
		this.pending = Buffer.concat([this.pending, frame]);
		this.flush(now);
		return true;
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
		this.pending = Buffer.alloc(0);
		this.rxTail = "";
		this.stats = null;
		this.lastRxAt = 0;
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
	let drainTimer: ReturnType<typeof setInterval> | null = null;
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
		if (drainTimer === null) {
			/* 持续排空 ESP 的下行：不读会把 tty 缓冲涨满、反向拖死 ESP 的 RX。
			 * 顺带把没发完的 pending 补发出去 */
			drainTimer = setInterval(() => {
				link.drain();
			}, DRAIN_MS);
			link.drain(); // 打开成功后立刻排一次，别等下一个周期
		}
	});

	pi.on("session_shutdown", async () => {
		if (timer !== null) {
			clearInterval(timer);
			timer = null;
		}
		if (drainTimer !== null) {
			clearInterval(drainTimer);
			drainTimer = null;
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
					const stats = link.espStats;
					let esp: string;
					if (stats === null) {
						esp = "，还没收到 ESP 下行（检查接线、或对方有没有在跑）";
					} else {
						/* ESP 心跳默认 10s 一次，超过 25s 没下行说明对端已经不发了 */
						const age = Math.max(0, Math.round((Date.now() - link.lastRxMs) / 1000));
						esp = `，ESP 心跳 ${stats.hbs} 次 / up ${stats.upSec}s / ${age}s 前有下行，rx=${stats.rx} bad=${stats.bad}`;
					}
					ctx.ui.notify(
						`状态栏：${state}${link.path ? ` ${link.path}` : "（未探测到串口）"}${esp}`,
						"info",
					);
					return;
				}
			}
		},
	});
}
