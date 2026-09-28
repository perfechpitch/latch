"""与 kernel_vu.c 的 gate_setup / add_setup 同一份 VU 静态组。

装进 .bachir 的 VUSTATIC，LoadBundle 开钟前 Preload。数值与 bach.h、
vu_types.h 同源。
"""

# 组内偏移与 opcode，对 kernel/bach.h
VU_STATIC_BASE = 0x1000
VU_STATIC_STRIDE = 0x100
VU_STATIC_DUP = 0x2C
VU_LU_OP = 0x00
VU_SU_OP = 0x04
VU_VALU0_OP = 0x08
VU_VALU1_OP = 0x0C
VU_VSFU_OP = 0x14
VU_PRF_OP = 0x2C
VU_TYPE_VL = 0x04
VU_VRF_RD_INDEX = 0x10
VU_VRF_WT_INDEX = 0x14

VU_LU_LD_BF16 = 0x03
VU_SU_NOP = 0x00
VU_SU_ST_MXFP8 = 0x02
VU_SU_ST_BF16 = 0x03
VU_VALU_FMUL_VV = 0x06
VU_VALU_FADD_VV = 0x01
VU_VSFU_SIGMOID = 0x04
VU_SRC_LU = 0x01
VU_SRC_VALU0 = 0x02
VU_SRC_VALU1 = 0x03
VU_SRC_VSFU = 0x05
VU_SRC_VRF_P0 = 0x30
VU_SRC_VRF_P1 = 0x31

MOE_SEG_INTER = 256
MOE_EMBED = 6144
RC_VRF = 16
VU_DATA_TYPE_SHIFT = 16
VRF_X = 0
VRF_SIG = 8          # ⌈MOE_SEG_INTER / 32⌉


def op_word(opcode, src1=0, src2=0):
    return opcode | (src1 << 8) | (src2 << 16)


def group_off(group, off):
    return VU_STATIC_BASE + group * VU_STATIC_STRIDE + off


def gate_writes():
    """组 1/2：组 1 写 x 与 sigmoid，组 2 两次乘后量化。"""
    type_vl = MOE_SEG_INTER
    vrf_wt = (VRF_SIG << 16) | VRF_X
    vrf_rd = (VRF_X << 16) | VRF_SIG
    w = []

    def st(group, off, data):
        w.append((group_off(group, off), data))

    st(1, VU_LU_OP, op_word(VU_LU_LD_BF16))
    st(1, VU_VSFU_OP, op_word(VU_VSFU_SIGMOID, VU_SRC_LU))
    st(1, VU_PRF_OP, VU_SRC_LU | (VU_SRC_VSFU << 8))
    st(1, VU_STATIC_DUP + VU_VRF_WT_INDEX, vrf_wt)
    st(1, VU_STATIC_DUP + VU_VRF_RD_INDEX, vrf_rd)
    st(1, VU_STATIC_DUP + VU_TYPE_VL, type_vl)

    st(2, VU_LU_OP, op_word(VU_LU_LD_BF16))
    st(2, VU_VALU0_OP, op_word(VU_VALU_FMUL_VV, VU_SRC_VRF_P0, VU_SRC_VRF_P1))
    st(2, VU_VALU1_OP, op_word(VU_VALU_FMUL_VV, VU_SRC_VALU0, VU_SRC_LU))
    st(2, VU_SU_OP, op_word(VU_SU_ST_MXFP8, VU_SRC_VALU1))
    st(2, VU_PRF_OP, 0)
    st(2, VU_STATIC_DUP + VU_VRF_WT_INDEX, vrf_wt)
    st(2, VU_STATIC_DUP + VU_VRF_RD_INDEX, vrf_rd)
    st(2, VU_STATIC_DUP + VU_TYPE_VL, type_vl)
    return w


def add_writes():
    """组 4/5：R core 两半 BF16 求和。"""
    type_vl = MOE_EMBED | (1 << VU_DATA_TYPE_SHIFT)
    w = []

    def st(group, off, data):
        w.append((group_off(group, off), data))

    st(4, VU_LU_OP, op_word(VU_LU_LD_BF16))
    st(4, VU_SU_OP, op_word(VU_SU_NOP))
    st(4, VU_PRF_OP, VU_SRC_LU)
    st(4, VU_STATIC_DUP + VU_VRF_WT_INDEX, RC_VRF)
    st(4, VU_STATIC_DUP + VU_TYPE_VL, type_vl)

    st(5, VU_LU_OP, op_word(VU_LU_LD_BF16))
    st(5, VU_VALU0_OP, op_word(VU_VALU_FADD_VV, VU_SRC_LU, VU_SRC_VRF_P0))
    st(5, VU_SU_OP, op_word(VU_SU_ST_BF16, VU_SRC_VALU0))
    st(5, VU_PRF_OP, 0)
    st(5, VU_STATIC_DUP + VU_VRF_RD_INDEX, RC_VRF)
    st(5, VU_STATIC_DUP + VU_TYPE_VL, type_vl)
    return w


def writes_for_chain(items):
    """按任务链上的符号拼本 core 要写的静态组。"""
    names = {it.sym[1] for it in items if it.sym}
    out = []
    if "task_vu_gate" in names:
        out.extend(gate_writes())
    if "task_vu_add" in names:
        out.extend(add_writes())
    return out
