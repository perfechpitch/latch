"""把一次 ksim 运行的观测产物压成回放页面。

读 ksim_run 写出来的三份 jsonl 与那份 run_meta.json，压成一个紧凑的 JSON 塞进
同目录的 template.html，页面写回那次运行的目录，跟它的观测产物放在一起。页面里每一格
颜色、每一个移动的点都来自这些时间戳，动画只在区间内做线性插值，不补任何没记下来的
中间状态。

它只认仿真跑完的那一份产物：拓扑、机器参数、层数都从 run_meta.json 里读，kernel 与
模型描述一概不碰。页面上那几段文字里的数也在这里算，所以换一次运行重跑一遍，图与文字
跟着一起变。

    python3 src/bach/ksim/make_playback.py <run_dir>   # 默认 build/playback
"""

import json
import pathlib
import sys
from collections import defaultdict

HERE = pathlib.Path(__file__).resolve().parent
PAGE_NAME = "simulation-playback.html"

UNITS = ["MC", "VC", "DTE"]        # 核里三条通道，停在 Recv 上那一段记在 waits 里
PORT_NAME = {2: "左口", 3: "右口", 4: "垂直口", 5: " PCIe 口"}
# 等的是谁：核自己停着，还是外围模块喂不进去
WAITER = {"host_credit": "外围模块"}
REASON_CN = {
    "incoming_barrier": "停在 Recv 上等对端的数据",
    "host_credit": "外围模块等额度还回来",
    "own_prev_task": "等自身前序任务",
    "downstream_credit": "等下游额度",
}


def read_jsonl(path):
    with open(path) as f:
        next(f)                     # 首行是 schema 声明
        return [json.loads(ln) for ln in f]


def num(x):
    return f"{x:,}"


def main(run_dir):
    run = pathlib.Path(run_dir)
    meta = json.loads((run / "run_meta.json").read_text())
    src = meta["source"]
    raw_spans = read_jsonl(run / "unit_spans.jsonl")
    raw_waits = read_jsonl(run / "unit_waits.jsonl")
    lat = read_jsonl(run / "global_latency.jsonl")

    chip_rows, chip_cols = (int(v) for v in src["chips"].split("x"))
    core_rows, core_cols = (int(v) for v in src["cores_per_chip"].split("x"))
    per_chip = core_rows * core_cols
    n_core = chip_rows * chip_cols * per_chip

    spans, links = [], []
    for s in raw_spans:
        d = s["end"] - s["start"]
        if s["unit"] == "ROUTER":
            links.append([s["node_id"], s["tid"], s["start"], d, s["volume"]])
        elif s["unit"] in UNITS:
            spans.append([s["node_id"], UNITS.index(s["unit"]), s["start"], d])
    spans.sort(key=lambda r: r[2])
    links.sort(key=lambda r: r[2])

    RECV, CREDIT = "incoming_barrier", "host_credit"
    reasons = sorted({w["reason"] for w in raw_waits})
    waits = sorted([[w["node_id"], w["start"], w["end"] - w["start"],
                     reasons.index(w["reason"])] for w in raw_waits],
                   key=lambda r: r[1])

    t_end = max([int(meta["result"]["end_time_ns"])]
                + [r[2] + r[3] for r in spans]
                + [r[1] + r[2] for r in waits]
                + [r[2] + r[3] for r in links])

    cores = sorted({r[0] for r in spans})
    layers = int(src.get("layers", 1))
    # 整个模型有多少层，用来判断这一次是不是截了几层。编译器带过来的，没有就当没截。
    full_layers = int(src.get("model_layers", layers))
    model_name = src.get("model_name", src.get("model", "这个模型"))

    # ---------------- 按时刻采样，找并发的峰值 ----------------
    step = max(1, t_end // 800)
    grid = list(range(0, t_end + step, step))
    busy = [0] * len(grid)
    waiting = [0] * len(grid)
    flying = [0] * len(grid)
    for c, u, st, d in spans:
        for k in range(st // step, min(len(grid) - 1, (st + d) // step) + 1):
            busy[k] += 1
    for c, st, d, r in waits:
        for k in range(st // step, min(len(grid) - 1, (st + d) // step) + 1):
            waiting[k] += 1
    for c, p, st, d, b in links:
        for k in range(st // step, min(len(grid) - 1, (st + d) // step) + 1):
            flying[k] += 1
    peak_busy = max(range(len(grid)), key=lambda k: busy[k])
    peak_fly = max(range(len(grid)), key=lambda k: flying[k])

    # ---------------- 表 ----------------
    wsum = defaultdict(lambda: [0, 0])
    for c, st, d, r in waits:
        wsum[reasons[r]][0] += d
        wsum[reasons[r]][1] += 1
    core_time = len(cores) * t_end
    wait_table = [[REASON_CN.get(k, k), WAITER.get(k, "核"), v[0], v[1],
                   f"{v[0] / core_time * 100:.1f}%"]
                  for k, v in sorted(wsum.items(), key=lambda kv: -kv[1][0])]

    lsum = defaultdict(lambda: [0, 0, 0])
    for c, p, st, d, b in links:
        e = lsum[(c, p)]
        e[0] += d
        e[1] += 1
        e[2] += b
    link_rank = sorted(lsum.items(), key=lambda kv: -kv[1][0])
    link_table = [[c, p, v[0], v[1], f"{v[2] / 1e6:.1f} MB"]
                  for (c, p), v in link_rank[:10]]

    # 结果出阵列的口：每个 chip 行最右边那个 chip 的右出口核，PCIe 口。进来的那一头
    # 不占出口，观测里记不到，所以这里数的是往外送的那一半。
    right_exit = core_cols - 1
    exits = {(r * chip_cols + chip_cols - 1) * per_chip + right_exit
             for r in range(chip_rows)}
    exit_busy = sum(v[0] for (c, p), v in lsum.items() if c in exits and p == 5)
    exit_top = max(((c, p, v) for (c, p), v in lsum.items()
                    if c in exits and p == 5), key=lambda x: x[2][0])
    inner_busy = sum(v[0] for (c, p), v in lsum.items() if p != 5)

    # 下面这些数只看核停在 Recv 上那一类，额度那一类是外围模块的事，混进来会算错
    recv_waits = [w for w in waits if reasons[w[3]] == RECV]
    credit_waits = [w for w in waits if reasons[w[3]] == CREDIT]

    # 每个核第一层收齐的时刻：第一个与最后一个之间差多少，就是入口串行化的代价
    first_wait_end, second_wait_end = {}, {}
    for c, st, d, r in recv_waits:
        if c not in first_wait_end:
            first_wait_end[c] = st + d
        elif c not in second_wait_end:
            second_wait_end[c] = st + d
    last_of_first = max(first_wait_end.values())
    first_of_second = min(second_wait_end.values()) if second_wait_end else last_of_first

    core_last = max([r[2] + r[3] for r in spans] + [r[1] + r[2] for r in recv_waits])
    compute = sum(r[3] for r in spans if r[1] in (0, 1))
    sending = sum(r[3] for r in spans if r[1] == 2)
    stalled = sum(r[2] for r in recv_waits)
    first_feed = min(g["start"] for g in lat)
    first_recv = min(r[1] + r[2] for r in recv_waits)
    first_core = min(((r[1] + r[2], r[0]) for r in recv_waits))[1]
    first_gemm = min(r[2] for r in spans if r[1] == 0)
    first_back = min(g["end"] for g in lat)
    half_back = sorted(g["end"] for g in lat)[len(lat) // 2]

    # ---------------- 值得停下来看的时刻 ----------------
    moments = [
        dict(at=first_feed, tag="注入", title="外围模块喂出第一份输入",
             short="外围模块开始往阵列里喂，核还什么都没收到。",
             text=f"时序表上第一条 FEED 在第 {num(first_feed)} 拍放行，"
                  f"这一拍喂出 {num(sum(1 for g in lat if g['start'] == first_feed))} 份。"
                  f"每个核手里有 {src['credit']} 份额度，喂满就得等核把上一份做完才还一份。"
                  f"此刻 {num(len(cores))} 个核全是空的。"),
        dict(at=first_recv, tag="首个收齐", title=f"core {first_core} 第一个收齐，开始算",
             short="第一个核收齐了输入，开始算；别的核还排在同一条链路后面。",
             text=f"从注入到这一拍隔了 {num(first_recv - first_feed)} 拍："
                  f"一份输入先过 {src['pcie_latency']} 拍的 PCIe 延迟，"
                  f"再按 {src['pcie_bandwidth']} 字节每拍推上链路，最后一跳跳走到目标核。"
                  f"收齐才发得出第一条 Gemm，一层是 Recv、两条 Gemm 出 gate 与 up、"
                  f"一条 swiglu、再一条 Gemm 出结果、Send，六条走完这一层就完了。"),
        dict(at=first_back, tag="首份回收", title="第一份结果回到外围模块",
             short="第一份结果回到外围模块，它要收齐一层的全部才能算下一层。",
             text=f"这一份从喂进去到回来用了 {num(first_back - first_feed)} 拍。"
                  f"回来的路跟去的路是同一条：结果要先走到本 chip 的左出口，再出 PCIe。"),
        dict(at=first_of_second, tag="下一层", title="有核开始收下一层的输入了",
             short="先做完的核开始收下一层的输入，这时还有核没收齐第一层的。",
             text="核里四条通道各走各的，一层的结果正往外推的同时，"
                  "下一层的输入可以正在收。激活区留了两块交替用，就是为了这个。"),
        dict(at=last_of_first, tag="末个收齐", title="最后一个核才收齐第一层的输入",
             short="到这一拍，全部核才都收齐第一层的输入。",
             text=f"第一个核在第 {num(first_recv)} 拍就收齐了，最后一个要等到这一拍，"
                  f"差了 {num(last_of_first - first_recv)} 拍。"
                  f"一个 chip 行的 {chip_cols * per_chip} 个核共用一条进出的链路，"
                  f"排在后面的只能等前面的传完。这一段差距就是整段时间的主要来源。"),
        dict(at=grid[peak_busy], tag="算得最满", title="同时在算的核最多",
             short=f"这一拍有 {busy[peak_busy]} 个核在算，是整段里最满的一刻。",
             text=f"{num(len(cores))} 个核里同时只有 {busy[peak_busy]} 个在算，"
                  f"剩下的都停在 Recv 上。计算不是这次运行的瓶颈，"
                  f"各核相加算了 {num(compute)} 拍，停着等了 {num(stalled)} 拍。"),
        dict(at=grid[peak_fly], tag="链路最挤", title="链路上同时在传的包最多",
             short=f"这一拍有 {flying[peak_fly]} 个包在链路上。",
             text=f"同一拍链路上有 {flying[peak_fly]} 个包在传。包不切片，"
                  f"一个大包整块占住一个出口，同一条链路上两个包不交错，"
                  f"后面的只能整包地排着。"),
        dict(at=half_back, tag="收回一半", title="一半的结果已经收回",
             short="回收过半，先做完的那些核已经空下来了。",
             text=f"{num(len(lat))} 份里的一半在第 {num(half_back)} 拍前回到外围模块。"
                  f"各核的长短不一样，因为路由分给它们的 token 数不同。"),
        dict(at=t_end, tag="收尾", title=f"{layers} 层走完，链路上最后一个包也到了",
             short=f"{layers} 层走完，阵列停下。",
             text=f"核上最后一条指令做完是第 {num(core_last)} 拍，此后阵列全空，"
                  f"但链路上还有包在飞，最后一个落到外围模块是第 {num(t_end)} 拍——"
                  f"隔了 {num(t_end - core_last)} 拍。计算做完与数据到齐不是同一时刻。"
                  f"端到端 {num(t_end)} 拍，平均一层 {num(t_end // layers)} 拍。"
                  + (f"照这个速度，整个模型落在核上的 {num(full_layers)} 层要 "
                     f"{num(t_end // layers * full_layers)} 拍。"
                     if layers < full_layers else "")),
    ]
    moments.sort(key=lambda m: m["at"])
    for m in moments:
        m["at"] = int(m["at"])

    scope = (
        f"这一份 kernel 是 {model_name} 的"
        f"{'前 ' + str(layers) + ' 层' if layers < full_layers else f'全部 {layers} 层'}"
        f"（落在核上的那些层）："
        f"{num(int(src['experts']))} 个专家一核一个铺在前 {num(len(cores))} 个核上，"
        f"一批 {num(int(src['seq']))} 个 token，"
        f"每个 token 走 {src.get('experts_per_token', '?')} 个专家，"
        f"路由按种子 {src['seed']} 求出来，所以每个核分到多少 token 是个确定的数。"
        f"整份跑完 {num(t_end)} 拍，各核相加算了 {num(compute)} 拍、"
        f"推数据出去 {num(sending)} 拍、停在 Recv 上 {num(stalled)} 拍。")

    gnote = (
        f"横向看得很清楚：每个核都是先一长条红色（停在 Recv 上等输入），"
        f"再三段蓝色（三条 Gemm）夹一小段黄色（swiglu），最后一小段绿色（Send）。"
        f"红色占了 {stalled / core_time * 100:.0f}%——这次运行里核绝大部分时间在等数据。")

    exit_core, _, exit_stat = exit_top
    inner = sorted(((v[0], c, p) for (c, p), v in lsum.items() if p != 5), reverse=True)
    inner_each = [x[0] for x in inner]
    inner_top = inner[0]
    off_exit = [x[0] for x in inner if x[1] not in exits]
    recv_row = next(r for r in wait_table if r[0] == REASON_CN[RECV])
    recv_share = recv_row[4]
    conclusion = (
        f"<p>两张表指向同一件事。核那边 {recv_share} 的核时长花在{recv_row[0]}上；"
        f"链路那边占用最重的那些口，全是每个 chip 行最右边那个 chip 的右出口核的 "
        f"PCIe 口，也就是这一行的 {chip_cols * per_chip} 个核送结果出去唯一的那条链路。"
        f"最忙的 core {exit_core} 那个口被占了 {num(exit_stat[0])} 拍，"
        f"是整段时间的 {exit_stat[0] / t_end * 100:.0f}%，"
        f"{exit_stat[2] / 1e6:.1f} MB 全从这一个口挤出去。"
        f"输入走的是另一边：从最左那一列 chip 的左出口进，跟它不抢。</p>"
        f"<p>片内那 {num(len(inner_each))} 条链路里最忙的是 core {inner_top[1]} 的"
        f"{PORT_NAME[inner_top[2]]}，占了 {inner_top[0] / t_end * 100:.0f}%——"
        f"它是回程上离出口最近的那一跳，堵的还是同一件事。"
        f"把出口核的那几条去掉，其余 {num(len(off_exit))} 条的中位数只有 "
        f"{sorted(off_exit)[len(off_exit) // 2] / t_end * 100:.0f}%。"
        f"进了阵列之后片内怎么走都不再是瓶颈，端到端几乎完全由每个 chip 行只有一条"
        f"对外链路这件事决定，跟核算得多快没多大关系。</p>"
        + ("<p>等待那张表里只有核停在 Recv 上这一类，说明额度从没扣光过：时序表本来"
           "就是按各层最长的那个核排的，不会喂过头。额度防的是表排错的时候。</p>"
           if not credit_waits else
           f"<p>等待还有第二类。外围模块有 {num(sum(w[2] for w in credit_waits))} 拍"
           f"卡在额度上：表说该喂，那个核手里的额度还没还回来，喂不进去。"
           f"额度就是这么把堵顶回外围模块的，一个核没额度不挡着别的核。</p>"))

    data = {
        "source": src,
        "tEnd": t_end,
        "units": UNITS,
        "reasons": reasons,
        "recvWait": reasons.index(RECV) if RECV in reasons else -1,
        "creditWait": reasons.index(CREDIT) if CREDIT in reasons else -1,
        "cores": cores,
        "layers": layers,
        "spans": spans,
        "waits": waits,
        "links": [[c, p, st, d] for c, p, st, d, b in links],
        "lat": [[g["uid"], g["start"], g["end"]] for g in lat],
        "counts": {"spans": len(raw_spans), "waits": len(raw_waits),
                   "links": len(links), "lat": len(lat)},
        "moments": moments,
        "waitTable": wait_table,
        "linkTable": link_table,
        "scope": scope,
        "gnote": gnote,
        "conclusion": conclusion,
    }

    html = (HERE / "template.html").read_text().replace(
        "/*DATA*/", json.dumps(data, separators=(",", ":"), ensure_ascii=False))
    page = run / PAGE_NAME
    page.write_text(html)
    print(f"核 {len(cores)}  span {len(spans)}  wait {len(waits)}  "
          f"link {len(links)}  tEnd {t_end}  →  {page}  {len(html) / 1024:.0f} KB")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/playback")
