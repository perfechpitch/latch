"""Tracetto：把 latch 的 waveform 按 user / task 分段看。

自己一套：自己的波形解码、自己的索引、自己的协议、自己的前端。不依赖仓库里别的 Python，
也不用第三方包。
"""

__all__ = ["reader", "spans", "index", "idxbuild", "server"]
