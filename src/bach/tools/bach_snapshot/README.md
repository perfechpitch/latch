# Bach 编译链快照

这里是从 Bach 仓库拷来的只读副本，供 `../export_bachir.py` 把一张 Map 编译成中间
文件。Map 编译规则不在 latch 这边重写，直接跑这份原件：四张辅助表的写入顺序与覆盖
规则、接收侧 task id 在编译期重放接收核展开规则算出的 tag、SKIP 行的上游 group 递归
收集，这几处照抄容易错，用原件才能保证两边跑的是同一份任务表。

**这里的文件不要改。** 要跟 Bach 的新版本对齐时，整份重新拷贝，然后重跑导出并比对
产出的中间文件。

## 来源

| 项 | 值 |
| --- | --- |
| 仓库 | `/home/colin/develop/bach` |
| commit | `3aad266510a21193a1b0ec9cc77366dfbb7af858` |
| 工作树状态 | 拷贝时有 38 个文件未提交，所以下面的逐文件校验值才是准的，commit 号只作参考 |
| 拷贝日期 | 2026-08-17 |

## 拷来的文件

```
0ad3a528075d1cd7b8c8ff1384673028bec7e645a866adb8e923d008f9eec671  hardware_config.py
ed55a0d320b64faa338385a9e5512d6b26a2eb6b3c99f527d1e80af6f1230e0a  topology_parser.py
0a6afcf6ee46f565c0d53c220cf72a9846405e7d3b7cc2a6ce6eb2ca5c5cfe55  config.py
a73476063f05c9c422e0f874cc74ff131deec5cff3f17975f64447d92a83d2f4  map_phaser.py
b9aa9f9433fce3b9d61a4daede2cc98268c5c244d5727e1b48a9c12a801e2f8f  map_tools/__init__.py
90af064fb9056e4e37c0f9f15dce17c449784939f76481cab8ee82281da1e674  map_tools/topology_mapper.py
bdec2845129d40a0fd7022d9509123536cbc19c5a787da57d5cf613a071b6c0e  map_tools/chip_rule_utils.py
```

导出脚本把这七个文件加上 `monitor.py` 的合成指纹写进中间文件的
`META compiler_sha256`，所以任何一份产出都能追回是哪一版编译链做的。

## 不是拷来的文件

`monitor.py` 是这边写的替身。原件有 200KB，它把 artifact_store 与 raw_stream 那一整套
atexit 落盘逻辑一起拉进来，而 Map 编译只在三处用到它，全是打快照日志。替身把快照开关
钉在关闭档，编译本身一步不受影响。

## 依赖

只用标准库。SimPy 与 pandas 那些都在运行时侧，编译链碰不到，所以系统 python3 直接能跑。
