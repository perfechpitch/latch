"""按一份拓扑描述算出全套硬件配置，写成一份 .bachir 产物。

描述给的是参数：阵列摆几颗 chip、每颗在哪一列、广播从哪进来、部分和按什么次序
归约。这一层把参数展开成硬件要的形式：每颗 chip 的 core_bad_mask、按 path_id 查
表的 RouterTable、TS 的位域任务链、每个 core 的角色、进核配置与全局项，再连同三
份 kernel 镜像一起放进 bundle。
"""

from . import bachir, kernelmap, moe, topology  # noqa: F401
