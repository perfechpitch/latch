"""kernel 符号表：一笔 task 的函数名换成它在 ITCM 里的入口地址。

`kernel/Makefile` 编完会在 `kernel/build/` 里留下 `kernel_<核>.sym`（一行“地址
类型 名字”）与 `kernel_<核>.hex`，后者由 gen_hwconfig 拷进各套 bundle。
task_chain 的 `TASK_PC` 直接取这里的地址，所以改了 kernel 的代码，重编一次
就够，编译器不需要跟着改。

三个 RV core 各自一份镜像、各自从 0 起，所以一笔 task 的 pc 取哪一份，看它
挂在哪个 RV core 上。
"""

from pathlib import Path

KERNEL_DIR = Path(__file__).resolve().parent.parent / "kernel"
BUILD_DIR = KERNEL_DIR / "build"

class KernelMap:
    """三份镜像的符号表。没编过就是空的，这时 pc 退回按固定步长排。"""

    def __init__(self):
        self.symbols = {}          # {(镜像, 函数名): 地址}
        self.images = {}           # {镜像: hex 文件路径}
        self.loaded = False

    def load(self, build_dir=None):
        base = Path(build_dir) if build_dir else BUILD_DIR
        for kind in ("dte", "mu", "vu"):
            sym = base / f"kernel_{kind}.sym"
            hexf = base / f"kernel_{kind}.hex"
            if not sym.exists():
                continue
            for line in sym.read_text(encoding="utf-8").splitlines():
                parts = line.split()
                if len(parts) == 3:
                    self.symbols[(kind, parts[2])] = int(parts[0], 16)
            if hexf.exists():
                self.images[kind] = hexf
        self.loaded = bool(self.symbols)
        return self
