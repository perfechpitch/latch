"""核阵列的形状换算。

一颗 chip 内的 core 用片内号定位，与硬件同一套；这里管的是把整个阵列看成一个
全局格子时的换算：全局坐标、chip 号与片内号之间怎么来回。
"""

class Dim:
    """核阵列的形状。

    每列 chip 的 core 列数可以不同：第一列与最后一列各多一列，放 B core、R core
    与不派角色的那个，中间几列没有它们。`core_cols_per_chip` 因此收一个整数
    （每列一样宽）或者一个每列一项的列表。全局列号是把各列 chip 的宽度接起来
    数出来的，`col_start` 记着每列 chip 从第几个全局列开始。
    """

    def __init__(self, chip_rows, chip_cols, core_rows_per_chip,
                 core_cols_per_chip, node_chip_rows, node_chip_cols):
        self.chip_rows = chip_rows
        self.chip_cols = chip_cols
        self.core_rows_per_chip = core_rows_per_chip
        if isinstance(core_cols_per_chip, int):
            self.core_cols = [core_cols_per_chip] * chip_cols
        else:
            self.core_cols = list(core_cols_per_chip)
            if len(self.core_cols) != chip_cols:
                raise ValueError("每列 chip 的 core 列数要给满 chip_cols 项")
        self.node_chip_rows = node_chip_rows
        self.node_chip_cols = node_chip_cols
        self.col_start = []
        at = 0
        for cols in self.core_cols:
            self.col_start.append(at)
            at += cols

    @property
    def core_cols_per_chip(self):
        """各列一样宽时的那个宽度。不等宽时没有这个数，取最大的那一列。"""
        return max(self.core_cols)

    @property
    def global_rows(self):
        return self.chip_rows * self.core_rows_per_chip

    @property
    def global_cols(self):
        return sum(self.core_cols)

    @property
    def core_num(self):
        return self.global_rows * self.global_cols

    def cores_of_chip(self, chip_col):
        return self.core_rows_per_chip * self.core_cols[chip_col]

    @property
    def cores_per_chip(self):
        """各列一样宽时每颗 chip 的 core 数。不等宽时取最大的那一档。"""
        return self.core_rows_per_chip * max(self.core_cols)

    def coord_of(self, core_id):
        return (core_id // self.global_cols, core_id % self.global_cols)

    def core_id_at(self, row, col):
        if row < 0 or col < 0 or row >= self.global_rows or col >= self.global_cols:
            return -1
        return row * self.global_cols + col

    def chip_col_of(self, col):
        """全局列号落在第几列 chip 上。"""
        for chip_col in range(self.chip_cols - 1, -1, -1):
            if col >= self.col_start[chip_col]:
                return chip_col
        return 0

    def chip_of_coord(self, row, col):
        return (row // self.core_rows_per_chip, self.chip_col_of(col))

    def chip_of(self, core_id):
        return self.chip_of_coord(*self.coord_of(core_id))

    def chip_id_of(self, core_id):
        chip_row, chip_col = self.chip_of(core_id)
        return chip_row * self.chip_cols + chip_col

    def local_id_of(self, core_id):
        """片内一维编号，行优先。与模型里 chip 内的 core 号同一套。"""
        row, col = self.coord_of(core_id)
        chip_col = self.chip_col_of(col)
        return ((row % self.core_rows_per_chip) * self.core_cols[chip_col] +
                (col - self.col_start[chip_col]))
