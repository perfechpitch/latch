class TopologyMapper:
    def __init__(self, chip_rows, chip_cols, core_rows_per_chip, core_cols_per_chip):
        self.chip_rows = chip_rows
        self.chip_cols = chip_cols
        self.core_rows_per_chip = core_rows_per_chip
        self.core_cols_per_chip = core_cols_per_chip
        
        # 计算全局的总行数和总列数
        self.global_rows = self.chip_rows * self.core_rows_per_chip
        self.global_cols = self.chip_cols * self.core_cols_per_chip

    # ==========================================
    # 第一层：绝对的真理 (核心转换)
    # ==========================================
    def id_to_global_coords(self, core_id: int) -> tuple[int, int]:
        """将全局一维 Core ID 转换为全局二维坐标 (abs_r, abs_c)"""
        abs_r = core_id // self.global_cols
        abs_c = core_id % self.global_cols
        return abs_r, abs_c

    def global_coords_to_id(self, abs_r: int, abs_c: int) -> int:
        """将全局二维坐标转回全局一维 Core ID"""
        return abs_r * self.global_cols + abs_c

    # ==========================================
    # 第二层：向下派生 (完全依赖第一层的真理)
    # ==========================================
    def global_coords_to_chip_coords(self, abs_r: int, abs_c: int) -> tuple[int, int]:
        """从全局坐标推导所属的 Chip 坐标"""
        chip_r = abs_r // self.core_rows_per_chip
        chip_c = abs_c // self.core_cols_per_chip
        return chip_r, chip_c

    def global_coords_to_local_coords(self, abs_r: int, abs_c: int) -> tuple[int, int]:
        """从全局坐标推导片内 Router/Core 坐标"""
        local_r = abs_r % self.core_rows_per_chip
        local_c = abs_c % self.core_cols_per_chip
        return local_r, local_c

    # ==========================================
    # 第三层：组合技 (你要的各种花式转换)
    # ==========================================
    def id_to_all_info(self, core_id: int) -> dict:
        """一步到位：输入 Core ID，吐出所有你想要的信息"""
        abs_r, abs_c = self.id_to_global_coords(core_id)
        chip_r, chip_c = self.global_coords_to_chip_coords(abs_r, abs_c)
        local_r, local_c = self.global_coords_to_local_coords(abs_r, abs_c)
        
        # 计算片内一维 ID (Local ID)
        local_id = local_r * self.core_cols_per_chip + local_c
        # 计算全局的 Chip ID
        chip_id = chip_r * self.chip_cols + chip_c
        
        return {
            "core_id": core_id,
            "global_coords": (abs_r, abs_c),
            "chip_id": chip_id,
            "chip_coords": (chip_r, chip_c),
            "local_id": local_id,
            "local_coords": (local_r, local_c)
        }

    def assemble_id(self, chip_coords: tuple[int, int], local_coords: tuple[int, int]) -> int:
        """从 Chip 坐标和 Local 坐标，反推回全局 Core ID"""
        chip_r, chip_c = chip_coords
        local_r, local_c = local_coords
        
        abs_r = chip_r * self.core_rows_per_chip + local_r
        abs_c = chip_c * self.core_cols_per_chip + local_c
        return self.global_coords_to_id(abs_r, abs_c)
