#!/usr/bin/env python3
"""`python3 src/utils/tracetto/tracetto.py …` 的入口。

把自己所在的那一级（`src/utils`）放进 sys.path，好让 `tracetto` 这个包能被 import
（同目录下的 selftest 也这么干）。在仓库根下 `python3 -m src.utils.tracetto …` 走的是
同一条 `__main__`。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tracetto.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
