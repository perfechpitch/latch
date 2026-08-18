"""编译期不需要观测，这里给 map_phaser 与它的同伴一个空壳。

真正的 monitor.py 有 200KB，它把 artifact_store 与 raw_stream 那一整套 atexit
落盘逻辑一起拉进来。而 Map 编译只在三处用到它，全是打快照日志：

    GLOBAL_MONITOR.take_snapshot(...)                       # 三个调用点
    getattr(GLOBAL_MONITOR, "_is_setup", False)             # 快照的开关
    getattr(GLOBAL_MONITOR, "registered_cores", [])         # 同上

_is_setup 保持 False，那个开关就一直关着，编译本身一步不受影响。

本文件不是从 bach 拷来的，是这边写的替身。同目录的其余文件才是快照。
"""


class LogLevel:
    FATAL = 0
    WARNING = 1
    EVENT = 2
    NOTE = 3
    INFO = 4
    DEBUG = 5
    TRACE = 6


SNAPSHOT_CORE_SOFTWARE_QUEUE = -4
SNAPSHOT_CORE_MOE_DISPATCHER = -5
SNAPSHOT_CORE_HOST = -1
SNAPSHOT_CORE_OUT = -2


class _NullMonitor:
    """空壳：快照全部丢弃，状态位保持在关闭档。"""

    _is_setup = False
    registered_cores = []

    def take_snapshot(self, *args, **kwargs):
        return None

    def __getattr__(self, name):
        def noop(*args, **kwargs):
            return None

        return noop


GLOBAL_MONITOR = _NullMonitor()
