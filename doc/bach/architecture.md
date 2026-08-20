# Bach 建模的最终形态

**文档模式：设计。** 供评审用，不作为实现依据。还没定的集中在最后一节。

本文定义整套东西的结构：一个模型怎么编译成每个核实际执行的 kernel，核阵列怎么跑它，
时间从哪里来。读者是要实现编译器和改模拟器的人。模型描述怎么写见《模型编译器》。

***

## 1. 整体

### 1.1 编译链路

三份输入进去，两份产出出来。编译器不估算计算时间，也不从形状反推，实测表里查不到的
才按权重加载量估。

```svg
<svg viewBox="0 0 900 430" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <defs>
    <marker id="f1a" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#555"/>
    </marker>
  </defs>

  <text x="95" y="24" text-anchor="middle" fill="#666" font-size="13">三份输入</text>
  <rect x="20" y="34" width="150" height="96" rx="3" fill="#fff" stroke="#888"/>
  <text x="32" y="52" fill="#234" font-weight="bold">config.json</text>
  <text x="32" y="70" fill="#666" font-size="11">hidden_size 7168</text>
  <text x="32" y="86" fill="#666" font-size="11">num_hidden_layers 61</text>
  <text x="32" y="102" fill="#666" font-size="11">n_routed_experts 384</text>
  <text x="32" y="118" fill="#666" font-size="11">moe_intermediate 3072</text>

  <rect x="20" y="146" width="150" height="96" rx="3" fill="#fff" stroke="#888"/>
  <text x="32" y="164" fill="#234" font-weight="bold">模型描述 .py</text>
  <text x="32" y="182" fill="#666" font-size="11">在核上   moe</text>
  <text x="32" y="198" fill="#666" font-size="11">不在核上 attention</text>
  <text x="32" y="214" fill="#666" font-size="11">         router  lm_head</text>
  <text x="32" y="230" fill="#666" font-size="11">分配     ALL[:384]</text>

  <rect x="20" y="258" width="150" height="70" rx="3" fill="#fff" stroke="#888"/>
  <text x="32" y="276" fill="#234" font-weight="bold">实测表</text>
  <text x="32" y="294" fill="#666" font-size="11">gemm/m/k/n/dtype</text>
  <text x="32" y="310" fill="#666" font-size="11">→ 拍数</text>
  <text x="32" y="324" fill="#999" font-size="10">查不到按权重加载估</text>

  <rect x="235" y="34" width="290" height="294" rx="4" fill="#eef3f8" stroke="#5b7fa6"/>
  <text x="380" y="56" text-anchor="middle" fill="#234" font-weight="bold">编译器</text>

  <g fill="#fff" stroke="#8aa8c4">
    <rect x="252" y="70" width="256" height="52" rx="3"/>
    <rect x="252" y="132" width="256" height="52" rx="3"/>
    <rect x="252" y="194" width="256" height="52" rx="3"/>
    <rect x="252" y="256" width="256" height="56" rx="3"/>
  </g>
  <text x="264" y="88" fill="#234">1  跑一遍模型描述</text>
  <text x="264" y="106" fill="#777" font-size="11">带 with 的算子记下来，其余只推形状</text>
  <text x="264" y="150" fill="#234">2  按种子求路由</text>
  <text x="264" y="168" fill="#777" font-size="11">4096 token 各选 6 个专家，每核数出 token 数</text>
  <text x="264" y="212" fill="#234">3  排地址</text>
  <text x="264" y="230" fill="#777" font-size="11">权重常驻排最前，激活区接在后面</text>
  <text x="264" y="274" fill="#234">4  降成四条指令</text>
  <text x="264" y="292" fill="#777" font-size="11">一个 ffn → Recv Gemm Gemm Elemwise Gemm Send</text>
  <text x="264" y="306" fill="#777" font-size="11">Gemm 与 Elemwise 查表填 cycles</text>

  <text x="700" y="24" text-anchor="middle" fill="#666" font-size="13">两份产出</text>
  <rect x="590" y="34" width="290" height="140" rx="3" fill="#eef8f0" stroke="#5ba677"/>
  <text x="604" y="54" fill="#243" font-weight="bold">kernel   384 段，每段 372 条</text>
  <g font-family="monospace" font-size="10.5" fill="#456">
    <text x="604" y="76">core 0</text>
    <text x="616" y="92">Recv  slave=0 dst=0x1F80000 len=1017856</text>
    <text x="616" y="107">Gemm  m=71 k=7168 n=3072 b=0x0     fp4</text>
    <text x="616" y="122">Gemm  m=71 k=7168 n=3072 b=0xA80000 fp4</text>
    <text x="616" y="137">Elem  swiglu n=218112 limit=10.0</text>
    <text x="616" y="152">Gemm  m=71 k=3072 n=7168 b=0x1500000 fp4</text>
    <text x="616" y="167">Send  len=1017856 dst_core=23 master=0</text>
  </g>

  <rect x="590" y="190" width="290" height="96" rx="3" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="604" y="210" fill="#432" font-weight="bold">时序表</text>
  <g font-family="monospace" font-size="10.5" fill="#654">
    <text x="604" y="232">Feed  cycle=8000 core=0 slave=0 len=1017856</text>
    <text x="604" y="250">Feed  cycle=8000 core=1 slave=0 len=960512</text>
    <text x="604" y="268">Drain core=0 master=0 len=1017856</text>
  </g>
  <text x="604" y="282" fill="#999" font-size="10">外围模块照它回放，本身没有逻辑</text>

  <g stroke="#555" fill="none" marker-end="url(#f1a)">
    <path d="M172,82 L231,110"/>
    <path d="M172,194 L231,170"/>
    <path d="M172,292 L231,282"/>
    <path d="M527,120 L586,104"/>
    <path d="M527,284 L586,236"/>
  </g>
</svg>
```

第二步那个种子决定了这一次编译的路由。同一个种子编出来的 kernel 每次一样，`m` 是个
确定的数，跑十次结果相同。

### 1.2 运行时

核只做 FFN。模型的其余部分在外围模块里，它照时序表把这一层的 FFN 输入喂进核阵列，
收齐结果再算下一层。61 层就是这两边交替走 61 遍。

```svg
<svg viewBox="0 0 900 400" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <defs>
    <marker id="f2a" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#555"/>
    </marker>
    <marker id="f2b" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#a6885b"/>
    </marker>
    <marker id="f2c" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#5ba677"/>
    </marker>
  </defs>

  <rect x="30" y="60" width="300" height="290" rx="4" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="180" y="84" text-anchor="middle" fill="#432" font-weight="bold">外围模块</text>
  <text x="180" y="102" text-anchor="middle" fill="#876" font-size="11">FFN 之外的全部计算</text>
  <g fill="#fff" stroke="#c0a97e">
    <rect x="50" y="116" width="120" height="30" rx="3"/>
    <rect x="190" y="116" width="120" height="30" rx="3"/>
    <rect x="50" y="156" width="120" height="30" rx="3"/>
    <rect x="190" y="156" width="120" height="30" rx="3"/>
    <rect x="50" y="196" width="120" height="30" rx="3"/>
    <rect x="190" y="196" width="120" height="30" rx="3"/>
  </g>
  <g fill="#543" text-anchor="middle" font-size="11">
    <text x="110" y="135">embedding</text>
    <text x="250" y="135">indexer  DSA</text>
    <text x="110" y="175">MLA attention</text>
    <text x="250" y="175">router</text>
    <text x="110" y="215">shared expert</text>
    <text x="250" y="215">lm_head  MTP</text>
  </g>
  <rect x="50" y="244" width="260" height="88" rx="3" fill="#fffdf8" stroke="#c0a97e" stroke-dasharray="3,3"/>
  <text x="180" y="264" text-anchor="middle" fill="#654" font-size="11">回放时序表</text>
  <g font-family="monospace" font-size="10" fill="#765">
    <text x="62" y="284">第 8000 拍  → core 0 slave0  1017856 B</text>
    <text x="62" y="300">第 8000 拍  → core 1 slave0   960512 B</text>
    <text x="62" y="316">           ...  384 个核</text>
  </g>

  <rect x="570" y="60" width="300" height="290" rx="4" fill="#eef8f0" stroke="#5ba677"/>
  <text x="720" y="84" text-anchor="middle" fill="#243" font-weight="bold">核阵列   512 核</text>
  <text x="720" y="102" text-anchor="middle" fill="#576" font-size="11">只跑 FFN，前 384 核各持一个专家</text>
  <g fill="#fff" stroke="#8cbf9f">
    <rect x="590" y="118" width="260" height="70" rx="3"/>
    <rect x="590" y="200" width="260" height="70" rx="3"/>
  </g>
  <text x="720" y="138" text-anchor="middle" fill="#243" font-size="11">routed expert  384 个</text>
  <g font-family="monospace" font-size="10" fill="#456">
    <text x="602" y="158">core 0   expert 0    71 token</text>
    <text x="602" y="174">core 1   expert 1    64 token</text>
    <text x="602" y="184">...      core 383   expert 383</text>
  </g>
  <text x="720" y="220" text-anchor="middle" fill="#243" font-size="11">一个核一层六条 kernel</text>
  <g font-family="monospace" font-size="10" fill="#456">
    <text x="602" y="240">Recv → Gemm → Gemm → Elem → Gemm → Send</text>
    <text x="602" y="258">33108 拍</text>
  </g>
  <rect x="590" y="282" width="260" height="50" rx="3" fill="#fbfefb" stroke="#8cbf9f" stroke-dasharray="3,3"/>
  <text x="720" y="302" text-anchor="middle" fill="#465" font-size="11">128 个核这一版空着</text>
  <text x="720" y="320" text-anchor="middle" fill="#798" font-size="10">384 与 512 不整除，没有再切</text>

  <path d="M334,150 L566,150" stroke="#a6885b" fill="none" marker-end="url(#f2b)" stroke-width="1.6"/>
  <text x="450" y="142" text-anchor="middle" fill="#876" font-size="11">这一层的 FFN 输入</text>
  <text x="450" y="166" text-anchor="middle" fill="#999" font-size="10">4096 token 按路由分给 384 个核</text>

  <path d="M566,250 L334,250" stroke="#5ba677" fill="none" marker-end="url(#f2c)" stroke-width="1.6"/>
  <text x="450" y="242" text-anchor="middle" fill="#576" font-size="11">FFN 结果</text>
  <text x="450" y="266" text-anchor="middle" fill="#999" font-size="10">384 份回到外围模块相加</text>

  <path d="M180,352 L180,378 L720,378 L720,354" stroke="#555" fill="none" marker-end="url(#f2a)" stroke-dasharray="4,3"/>
  <text x="450" y="394" text-anchor="middle" fill="#666" font-size="11">一层走完接着下一层，61 层</text>

  <text x="450" y="34" text-anchor="middle" fill="#333" font-size="13">一次前向：外围模块与核阵列交替走 61 遍</text>
</svg>
```

***

## 2. 硬件

### 2.1 核阵列与核对外的口

16 行 4 列个 chip，每个 chip 2 行 4 列个核，每个核里一个 Router。Router 连左邻、右邻、
垂直邻居和本核，边上的核再接一个 PCIe 口。所有连线都是双向的，一条双向连接的两端各有
一个 master 和一个 slave。

```svg
<svg viewBox="0 0 900 730" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <defs>
    <marker id="g3a" markerWidth="7" markerHeight="7" refX="6" refY="2.5" orient="auto">
      <path d="M0,0 L6,2.5 L0,5 z" fill="#5b7fa6"/>
    </marker>
    <marker id="g3s" markerWidth="7" markerHeight="7" refX="1" refY="2.5" orient="auto">
      <path d="M6,0 L0,2.5 L6,5 z" fill="#5b7fa6"/>
    </marker>
    <marker id="g3p" markerWidth="7" markerHeight="7" refX="6" refY="2.5" orient="auto">
      <path d="M0,0 L6,2.5 L0,5 z" fill="#c07"/>
    </marker>
    <marker id="g3q" markerWidth="7" markerHeight="7" refX="1" refY="2.5" orient="auto">
      <path d="M6,0 L0,2.5 L6,5 z" fill="#c07"/>
    </marker>
  </defs>

  <text x="450" y="22" text-anchor="middle" fill="#333" font-size="13">chip 阵列   4 列是全部，行画了前 4 行，一共 16 行   所有连线都是双向的</text>

  <rect x="18" y="40" width="26" height="292" rx="3" fill="#dfeee2" stroke="#6a9"/>
  <text x="31" y="186" text-anchor="middle" fill="#375" font-size="10" transform="rotate(-90 31 186)">PCIe Switch</text>
  <rect x="856" y="40" width="26" height="292" rx="3" fill="#dfeee2" stroke="#6a9"/>
  <text x="869" y="186" text-anchor="middle" fill="#375" font-size="10" transform="rotate(-90 869 186)">PCIe Switch</text>

  <g stroke="#999" fill="#f4f4f4">
    <rect x="70" y="46" width="152" height="58" rx="4"/><rect x="272" y="46" width="152" height="58" rx="4"/><rect x="474" y="46" width="152" height="58" rx="4"/><rect x="676" y="46" width="152" height="58" rx="4"/>
    <rect x="70" y="122" width="152" height="58" rx="4"/><rect x="272" y="122" width="152" height="58" rx="4"/><rect x="474" y="122" width="152" height="58" rx="4"/><rect x="676" y="122" width="152" height="58" rx="4"/>
    <rect x="70" y="198" width="152" height="58" rx="4"/><rect x="272" y="198" width="152" height="58" rx="4"/><rect x="474" y="198" width="152" height="58" rx="4"/><rect x="676" y="198" width="152" height="58" rx="4"/>
    <rect x="70" y="274" width="152" height="58" rx="4"/><rect x="272" y="274" width="152" height="58" rx="4"/><rect x="474" y="274" width="152" height="58" rx="4"/><rect x="676" y="274" width="152" height="58" rx="4"/>
  </g>
  <g fill="#666" text-anchor="middle" font-size="10">
    <text x="146" y="70">chip 0</text><text x="348" y="70">chip 1</text><text x="550" y="70">chip 2</text><text x="752" y="70">chip 3</text>
    <text x="146" y="146">chip 4</text><text x="348" y="146">chip 5</text><text x="550" y="146">chip 6</text><text x="752" y="146">chip 7</text>
    <text x="146" y="222">chip 8</text><text x="348" y="222">chip 9</text><text x="550" y="222">chip 10</text><text x="752" y="222">chip 11</text>
    <text x="146" y="298">chip 12</text><text x="348" y="298">chip 13</text><text x="550" y="298">chip 14</text><text x="752" y="298">chip 15</text>
    <text x="146" y="88">core 0-7</text><text x="348" y="88">core 8-15</text><text x="550" y="88">core 16-23</text><text x="752" y="88">core 24-31</text>
    <text x="146" y="164">core 32-39</text><text x="348" y="164">core 40-47</text><text x="550" y="164">core 48-55</text><text x="752" y="164">core 56-63</text>
    <text x="146" y="240">core 64-71</text><text x="348" y="240">core 72-79</text><text x="550" y="240">core 80-87</text><text x="752" y="240">core 88-95</text>
    <text x="146" y="316">core 96-103</text><text x="348" y="316">core 104-111</text><text x="550" y="316">core 112-119</text><text x="752" y="316">core 120-127</text>
  </g>
  <g fill="#e6d4ee" stroke="#96c">
    <rect x="58" y="66" width="12" height="18" rx="2"/><rect x="222" y="66" width="12" height="18" rx="2"/><rect x="260" y="66" width="12" height="18" rx="2"/><rect x="424" y="66" width="12" height="18" rx="2"/><rect x="462" y="66" width="12" height="18" rx="2"/><rect x="626" y="66" width="12" height="18" rx="2"/><rect x="664" y="66" width="12" height="18" rx="2"/><rect x="828" y="66" width="12" height="18" rx="2"/>
    <rect x="58" y="142" width="12" height="18" rx="2"/><rect x="222" y="142" width="12" height="18" rx="2"/><rect x="260" y="142" width="12" height="18" rx="2"/><rect x="424" y="142" width="12" height="18" rx="2"/><rect x="462" y="142" width="12" height="18" rx="2"/><rect x="626" y="142" width="12" height="18" rx="2"/><rect x="664" y="142" width="12" height="18" rx="2"/><rect x="828" y="142" width="12" height="18" rx="2"/>
    <rect x="58" y="218" width="12" height="18" rx="2"/><rect x="222" y="218" width="12" height="18" rx="2"/><rect x="260" y="218" width="12" height="18" rx="2"/><rect x="424" y="218" width="12" height="18" rx="2"/><rect x="462" y="218" width="12" height="18" rx="2"/><rect x="626" y="218" width="12" height="18" rx="2"/><rect x="664" y="218" width="12" height="18" rx="2"/><rect x="828" y="218" width="12" height="18" rx="2"/>
    <rect x="58" y="294" width="12" height="18" rx="2"/><rect x="222" y="294" width="12" height="18" rx="2"/><rect x="260" y="294" width="12" height="18" rx="2"/><rect x="424" y="294" width="12" height="18" rx="2"/><rect x="462" y="294" width="12" height="18" rx="2"/><rect x="626" y="294" width="12" height="18" rx="2"/><rect x="664" y="294" width="12" height="18" rx="2"/><rect x="828" y="294" width="12" height="18" rx="2"/>
    <rect x="137" y="36" width="18" height="10" rx="2"/><rect x="137" y="104" width="18" height="10" rx="2"/><rect x="339" y="36" width="18" height="10" rx="2"/><rect x="339" y="104" width="18" height="10" rx="2"/><rect x="541" y="36" width="18" height="10" rx="2"/><rect x="541" y="104" width="18" height="10" rx="2"/><rect x="743" y="36" width="18" height="10" rx="2"/><rect x="743" y="104" width="18" height="10" rx="2"/>
    <rect x="137" y="112" width="18" height="10" rx="2"/><rect x="137" y="180" width="18" height="10" rx="2"/><rect x="339" y="112" width="18" height="10" rx="2"/><rect x="339" y="180" width="18" height="10" rx="2"/><rect x="541" y="112" width="18" height="10" rx="2"/><rect x="541" y="180" width="18" height="10" rx="2"/><rect x="743" y="112" width="18" height="10" rx="2"/><rect x="743" y="180" width="18" height="10" rx="2"/>
    <rect x="137" y="188" width="18" height="10" rx="2"/><rect x="137" y="256" width="18" height="10" rx="2"/><rect x="339" y="188" width="18" height="10" rx="2"/><rect x="339" y="256" width="18" height="10" rx="2"/><rect x="541" y="188" width="18" height="10" rx="2"/><rect x="541" y="256" width="18" height="10" rx="2"/><rect x="743" y="188" width="18" height="10" rx="2"/><rect x="743" y="256" width="18" height="10" rx="2"/>
    <rect x="137" y="264" width="18" height="10" rx="2"/><rect x="137" y="332" width="18" height="10" rx="2"/><rect x="339" y="264" width="18" height="10" rx="2"/><rect x="339" y="332" width="18" height="10" rx="2"/><rect x="541" y="264" width="18" height="10" rx="2"/><rect x="541" y="332" width="18" height="10" rx="2"/><rect x="743" y="264" width="18" height="10" rx="2"/><rect x="743" y="332" width="18" height="10" rx="2"/>
  </g>
  <g stroke="#c07" fill="none" stroke-width="1.3" marker-end="url(#g3p)" marker-start="url(#g3q)">
    <path d="M46,75 L56,75"/><path d="M236,75 L258,75"/><path d="M438,75 L460,75"/><path d="M640,75 L662,75"/><path d="M844,75 L854,75"/>
    <path d="M46,151 L56,151"/><path d="M236,151 L258,151"/><path d="M438,151 L460,151"/><path d="M640,151 L662,151"/><path d="M844,151 L854,151"/>
    <path d="M46,227 L56,227"/><path d="M236,227 L258,227"/><path d="M438,227 L460,227"/><path d="M640,227 L662,227"/><path d="M844,227 L854,227"/>
    <path d="M46,303 L56,303"/><path d="M236,303 L258,303"/><path d="M438,303 L460,303"/><path d="M640,303 L662,303"/><path d="M844,303 L854,303"/>
    <path d="M146,114 L146,112"/><path d="M348,114 L348,112"/><path d="M550,114 L550,112"/><path d="M752,114 L752,112"/>
    <path d="M146,190 L146,188"/><path d="M348,190 L348,188"/><path d="M550,190 L550,188"/><path d="M752,190 L752,188"/>
    <path d="M146,266 L146,264"/><path d="M348,266 L348,264"/><path d="M550,266 L550,264"/><path d="M752,266 L752,264"/>
  </g>
  <g fill="#bbb" text-anchor="middle" font-size="13">
    <text x="146" y="352">. . .</text><text x="348" y="352">. . .</text><text x="550" y="352">. . .</text><text x="752" y="352">. . .</text>
  </g>
  <text x="450" y="370" text-anchor="middle" fill="#96c" font-size="10">紫色小块是 chip 的四个 PCIe 口，左右接相邻列，上下接相邻行，最外接 PCIe Switch</text>

  <line x1="40" y1="390" x2="860" y2="390" stroke="#ddd"/>
  <text x="450" y="416" text-anchor="middle" fill="#333" font-size="13">一个 chip   2 行 x 4 列，每个 core 里一个 Router，两排 Router 朝内</text>

  <rect x="120" y="432" width="660" height="230" rx="5" fill="#fbfbfb" stroke="#999"/>

  <g stroke="#5ba677" fill="#eef8f0">
    <rect x="150" y="472" width="130" height="72" rx="4"/><rect x="300" y="472" width="130" height="72" rx="4"/>
    <rect x="450" y="472" width="130" height="72" rx="4"/><rect x="600" y="472" width="130" height="72" rx="4"/>
    <rect x="150" y="562" width="130" height="72" rx="4"/><rect x="300" y="562" width="130" height="72" rx="4"/>
    <rect x="450" y="562" width="130" height="72" rx="4"/><rect x="600" y="562" width="130" height="72" rx="4"/>
  </g>
  <g fill="#243" font-size="11" text-anchor="middle">
    <text x="215" y="490">core 0</text><text x="365" y="490">core 1</text><text x="515" y="490">core 2</text><text x="665" y="490">core 3</text>
    <text x="215" y="628">core 4</text><text x="365" y="628">core 5</text><text x="515" y="628">core 6</text><text x="665" y="628">core 7</text>
  </g>
  <g stroke="#5b7fa6" fill="#dde9f4">
    <rect x="180" y="508" width="70" height="30" rx="3"/><rect x="330" y="508" width="70" height="30" rx="3"/>
    <rect x="480" y="508" width="70" height="30" rx="3"/><rect x="630" y="508" width="70" height="30" rx="3"/>
    <rect x="180" y="568" width="70" height="30" rx="3"/><rect x="330" y="568" width="70" height="30" rx="3"/>
    <rect x="480" y="568" width="70" height="30" rx="3"/><rect x="630" y="568" width="70" height="30" rx="3"/>
  </g>
  <g fill="#234" font-size="9.5" text-anchor="middle">
    <text x="215" y="527">Router</text><text x="365" y="527">Router</text><text x="515" y="527">Router</text><text x="665" y="527">Router</text>
    <text x="215" y="587">Router</text><text x="365" y="587">Router</text><text x="515" y="587">Router</text><text x="665" y="587">Router</text>
  </g>

  <g stroke="#5b7fa6" fill="none" stroke-width="1.5" marker-end="url(#g3a)" marker-start="url(#g3s)">
    <path d="M252,523 L328,523"/><path d="M402,523 L478,523"/><path d="M552,523 L628,523"/>
    <path d="M252,583 L328,583"/><path d="M402,583 L478,583"/><path d="M552,583 L628,583"/>
    <path d="M215,540 L215,566"/><path d="M365,540 L365,566"/>
    <path d="M515,540 L515,566"/><path d="M665,540 L665,566"/>
  </g>

  <g fill="#e6d4ee" stroke="#96c">
    <rect x="106" y="570" width="16" height="26" rx="2"/>
    <rect x="786" y="510" width="16" height="26" rx="2"/>
    <rect x="202" y="422" width="26" height="14" rx="2"/>
    <rect x="652" y="670" width="26" height="14" rx="2"/>
  </g>
  <g fill="#85a" font-size="9.5" text-anchor="middle">
    <text x="114" y="562">左</text><text x="794" y="502">右</text>
    <text x="215" y="418">上</text><text x="665" y="696">下</text>
  </g>

  <g stroke="#96c" fill="none" stroke-width="1.5" marker-end="url(#g3p)" marker-start="url(#g3q)">
    <path d="M124,583 L178,583"/>
    <path d="M215,438 L215,506"/>
    <path d="M784,523 L702,523"/>
    <path d="M665,668 L665,600"/>
  </g>

  <g fill="#96c" font-size="10">
    <text x="126" y="610">左口 → core 4</text>
    <text x="234" y="456">上口 → core 0</text>
    <text x="700" y="502">右口 → core 3</text>
    <text x="576" y="686">下口 → core 7</text>
  </g>

  <text x="450" y="718" text-anchor="middle" fill="#5b7fa6" font-size="11">Router 连左邻、右邻、同列的垂直邻居和本核，全部双向；四个 PCIe 口分别接在四个角上的核：上接 core 0，右接 core 3，左接 core 4，下接 core 7</text>
</svg>
```

一个 chip 四个 PCIe 口：左右接相邻列的 chip，上下接相邻行的 chip，最外侧接 PCIe
Switch。四个口分别接在四个角上的核：上口接 core 0，右口接 core 3，左口接 core 4，下口接 core 7。

`Send` 同时写目标核和走哪个口：口决定这一包交给 Router 的哪个本地入口，目标核决定
Router 往哪条线上送。`Recv` 只写走哪个口，从谁那里收由连线决定。

### 2.2 一个核的内部

八个单元跟着同一个时钟协程走。

```svg
<svg viewBox="0 0 900 620" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <defs>
    <marker id="f4a" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#5b7fa6"/>
    </marker>
    <marker id="f4b" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#999"/>
    </marker>
    <marker id="f4c" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#a6885b"/>
    </marker>
    <marker id="f4d" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#5ba677"/>
    </marker>
  </defs>

  <rect x="20" y="16" width="860" height="540" rx="5" fill="none" stroke="#999" stroke-dasharray="5,4"/>
  <text x="36" y="38" fill="#666" font-size="13">Core   八个单元跑在同一个时钟协程里</text>

  <rect x="45" y="58" width="150" height="76" rx="3" fill="#eef3f8" stroke="#5b7fa6"/>
  <text x="120" y="82" text-anchor="middle" fill="#234" font-weight="bold">TaskScheduler</text>
  <text x="120" y="100" text-anchor="middle" fill="#678" font-size="10">按 kernel 类型分发</text>
  <text x="120" y="116" text-anchor="middle" fill="#678" font-size="10">收齐本拍 ack 再派下一批</text>

  <g fill="#fff" stroke="#5b7fa6">
    <rect x="290" y="56" width="180" height="46" rx="3"/>
    <rect x="290" y="112" width="180" height="46" rx="3"/>
  </g>
  <text x="304" y="76" fill="#234" font-weight="bold" font-size="11">MatrixCore</text>
  <text x="304" y="93" fill="#777" font-size="10">Gemm   按 cycles 计时</text>
  <text x="304" y="132" fill="#234" font-weight="bold" font-size="11">VectorCore</text>
  <text x="304" y="149" fill="#777" font-size="10">Elemwise  swiglu</text>

  <rect x="290" y="172" width="180" height="96" rx="3" fill="#fff" stroke="#5ba677"/>
  <text x="304" y="192" fill="#243" font-weight="bold" font-size="11">DTE   两个方向</text>
  <text x="304" y="210" fill="#777" font-size="10">出  Send  逐拍推出去，完了 ack</text>
  <text x="304" y="226" fill="#777" font-size="10">入  Recv  攒齐整包才处理</text>
  <text x="304" y="244" fill="#777" font-size="10">四段闸门：准入 setup 通道 功能单元</text>
  <text x="304" y="260" fill="#999" font-size="10">RETIRE 收包不过闸门，否则死锁</text>

  <rect x="290" y="282" width="180" height="52" rx="3" fill="#fff" stroke="#888"/>
  <text x="304" y="302" fill="#234" font-weight="bold" font-size="11">CreditUnit</text>
  <text x="304" y="319" fill="#777" font-size="10">查账锁 → 查账 4 拍 → 各下游扣一份</text>

  <g stroke="#5b7fa6" fill="none" marker-end="url(#f4a)">
    <path d="M197,72 L286,74"/>
    <path d="M197,84 L286,130"/>
    <path d="M197,96 L286,196"/>
    <path d="M197,110 L286,300"/>
  </g>
  <text x="238" y="60" fill="#5b7fa6" font-size="10">派</text>

  <g stroke="#999" fill="none" marker-end="url(#f4b)" stroke-dasharray="3,2">
    <path d="M286,322 L250,322 L250,140 L197,128"/>
  </g>
  <text x="212" y="150" fill="#999" font-size="10">ack</text>

  <rect x="45" y="330" width="200" height="200" rx="3" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="145" y="352" text-anchor="middle" fill="#432" font-weight="bold">MemorySystem</text>
  <text x="145" y="368" text-anchor="middle" fill="#876" font-size="10">一层 SRAM，权重常驻</text>

  <rect x="60" y="380" width="170" height="66" rx="2" fill="#fff" stroke="#c0a97e"/>
  <text x="145" y="398" text-anchor="middle" fill="#654" font-size="11">CoreMem   一个仲裁器</text>
  <g font-family="monospace" font-size="9.5" fill="#876" text-anchor="middle">
    <text x="145" y="416">Port_DTE_CM</text>
    <text x="145" y="430">Port_Vector   Port_Matrix_CM</text>
  </g>
  <rect x="60" y="456" width="170" height="58" rx="2" fill="#fff" stroke="#c0a97e"/>
  <text x="145" y="474" text-anchor="middle" fill="#654" font-size="11">MatrixMem   一个仲裁器</text>
  <g font-family="monospace" font-size="9.5" fill="#876" text-anchor="middle">
    <text x="145" y="492">Port_DTE_MM   Port_Matrix_MM</text>
  </g>
  <text x="145" y="524" text-anchor="middle" fill="#987" font-size="9.5">同一块上的口互相排队，跨块的互不干扰</text>

  <g stroke="#a6885b" fill="none" stroke-dasharray="3,3" marker-end="url(#f4c)">
    <path d="M286,84 L262,84 L262,486 L234,486"/>
    <path d="M286,140 L270,140 L270,424 L234,424"/>
    <path d="M286,220 L278,220 L278,410 L234,410"/>
  </g>
  <text x="252" y="360" fill="#a6885b" font-size="10">访存</text>

  <rect x="560" y="120" width="180" height="200" rx="3" fill="#eef8f0" stroke="#5ba677"/>
  <text x="650" y="144" text-anchor="middle" fill="#243" font-weight="bold">Router</text>
  <text x="650" y="162" text-anchor="middle" fill="#576" font-size="10">每拍：投递 → 收包 → 仲裁</text>
  <g fill="#fff" stroke="#8cbf9f">
    <rect x="576" y="176" width="148" height="26" rx="2"/>
    <rect x="576" y="208" width="148" height="26" rx="2"/>
    <rect x="576" y="240" width="148" height="26" rx="2"/>
    <rect x="576" y="272" width="148" height="26" rx="2"/>
  </g>
  <g fill="#465" font-size="10" text-anchor="middle">
    <text x="650" y="193">本核   口 0   口 1</text>
    <text x="650" y="225">左邻       右邻</text>
    <text x="650" y="257">垂直邻居</text>
    <text x="650" y="289">PCIe   只有边上的核有</text>
  </g>
  <text x="650" y="314" text-anchor="middle" fill="#798" font-size="9.5">每条都是双向，各口抢同一个出口</text>

  <path d="M474,196 L556,186" stroke="#5ba677" fill="none" marker-end="url(#f4d)" stroke-width="1.5"/>
  <text x="515" y="178" text-anchor="middle" fill="#5ba677" font-size="10">口 0   master 发 slave 收</text>
  <path d="M556,232 L474,222" stroke="#5ba677" fill="none" marker-end="url(#f4d)" stroke-width="1.5"/>
  <text x="515" y="248" text-anchor="middle" fill="#5ba677" font-size="10">口 1   同样双向</text>

  <path d="M744,220 L840,220" stroke="#5ba677" fill="none" marker-end="url(#f4d)" stroke-width="1.5"/>
  <text x="800" y="210" text-anchor="middle" fill="#576" font-size="10">片内 mesh</text>
  <text x="800" y="240" text-anchor="middle" fill="#576" font-size="10">片间 PCIe</text>

  <rect x="560" y="360" width="300" height="170" rx="3" fill="#fff" stroke="#888"/>
  <text x="574" y="382" fill="#234" font-weight="bold" font-size="12">每拍的顺序   末级先做</text>
  <g font-size="11" fill="#555">
    <text x="586" y="404">1  router    到点的包重组齐了交给 DTE，仲裁转发</text>
    <text x="586" y="424">2  memory    访存到点的释放仲裁器</text>
    <text x="586" y="444">3  mc        算完的 ack 出去</text>
    <text x="586" y="464">4  vc        同上</text>
    <text x="586" y="484">5  dte       发完收完 ack，RETIRE 把额度还给 cu</text>
    <text x="586" y="504">6  cu        拿刚还回的额度验资，验过的 ack</text>
    <text x="586" y="524">7  ts        看进本拍全部 ack，派下一批</text>
  </g>

  <text x="36" y="578" fill="#666" font-size="11">ts 排最后，所以一次完成与它引发的连锁落在同一拍；代价是它本拍派下去的任务各单元下一拍才动。</text>
  <text x="36" y="598" fill="#666" font-size="11">router 排在 dte 之前，同样的取舍：本拍投递到的包 dte 本拍就处理，dte 本拍发出的包 router 下一拍才仲裁。</text>
</svg>
```

存储分两块，各自一个仲裁器。口自带带宽，一次访问的拍数是字节数除以带宽向上取整。

***

## 3. kernel

### 3.1 四条指令

核只做 FFN，所以指令只有四条。

| 单元 | 指令 | 字段 |
| --- | --- | --- |
| MC | `Gemm` | `m` `k` `n`，操作数地址 `a` `b` `c`，`dtype` 是 B 的位宽，`cycles` |
| VC | `Elemwise` | `op` `n`，源地址表 `srcs`，`dst`，`limit`，`cycles` |
| DTE | `Send` | `src` `length` `dst_core` `master` |
| DTE | `Recv` | `slave` `dst` `length` |

`Gemm` 一条覆盖所有矩阵乘，QK 与 PV 是换操作数，fp4 与 fp8 是换 `dtype`。

### 3.2 地址怎么排

一层 SRAM，编译器静态排好每块数据的地址。权重常驻，排在最前；激活区接在权重后面。
同一个核上同名的一段权重只占一次。

***

## 4. 数据流

一次前向里这一层走一遍：外围模块按路由把 token 分给 384 个核，每个核在自己的 SRAM
里过三个矩阵，结果发回去相加。

```svg
<svg viewBox="0 0 900 520" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <defs>
    <marker id="f5a" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#5b7fa6"/>
    </marker>
    <marker id="f5b" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto">
      <path d="M0,0 L8,3 L0,6 z" fill="#a6885b"/>
    </marker>
  </defs>

  <rect x="20" y="40" width="140" height="180" rx="3" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="90" y="62" text-anchor="middle" fill="#432" font-weight="bold" font-size="11">外围模块</text>
  <text x="90" y="82" text-anchor="middle" fill="#765" font-size="10">attention 算完</text>
  <text x="90" y="100" text-anchor="middle" fill="#765" font-size="10">router 选完专家</text>
  <rect x="34" y="112" width="112" height="46" rx="2" fill="#fff" stroke="#c0a97e"/>
  <text x="90" y="130" text-anchor="middle" fill="#543" font-size="10">4096 token</text>
  <text x="90" y="146" text-anchor="middle" fill="#543" font-size="10">7168 维  bf16</text>
  <text x="90" y="178" text-anchor="middle" fill="#876" font-size="10">每个 token 走 6 个专家</text>
  <text x="90" y="196" text-anchor="middle" fill="#876" font-size="10">4096 x 6 / 384</text>
  <text x="90" y="212" text-anchor="middle" fill="#876" font-size="10">平均每核 64 个</text>

  <g stroke="#a6885b" fill="none" marker-end="url(#f5b)">
    <path d="M164,90 L214,66"/><path d="M164,110 L214,136"/>
    <path d="M164,140 L214,206"/><path d="M164,170 L214,306"/>
  </g>
  <text x="188" y="242" fill="#a6885b" font-size="10">按路由分发</text>

  <g stroke="#5ba677" fill="#eef8f0">
    <rect x="220" y="46" width="128" height="42" rx="3"/>
    <rect x="220" y="116" width="128" height="42" rx="3"/>
    <rect x="220" y="186" width="128" height="42" rx="3"/>
    <rect x="220" y="286" width="128" height="42" rx="3"/>
  </g>
  <g font-size="10" fill="#243">
    <text x="232" y="64">core 0   expert 0</text><text x="232" y="80" fill="#798">71 token  1017856 B</text>
    <text x="232" y="134">core 1   expert 1</text><text x="232" y="150" fill="#798">64 token   917504 B</text>
    <text x="232" y="204">core 2   expert 2</text><text x="232" y="220" fill="#798">67 token   960512 B</text>
    <text x="232" y="304">core 383 expert 383</text><text x="232" y="320" fill="#798">70 token  1003520 B</text>
  </g>
  <text x="284" y="258" text-anchor="middle" fill="#aaa" font-size="14">. . .</text>

  <rect x="400" y="40" width="330" height="290" rx="4" fill="#fbfbfb" stroke="#888"/>
  <text x="565" y="62" text-anchor="middle" fill="#333" font-size="12">一个核里数据怎么走</text>
  <text x="565" y="78" text-anchor="middle" fill="#888" font-size="10">一层 SRAM，编译器静态排好地址</text>

  <g stroke="#c0a97e" fill="#fdf9f2">
    <rect x="420" y="92" width="150" height="24" rx="2"/>
    <rect x="420" y="120" width="150" height="24" rx="2"/>
    <rect x="420" y="148" width="150" height="24" rx="2"/>
  </g>
  <g font-family="monospace" font-size="9.5" fill="#654">
    <text x="428" y="108">0x0000000  W_gate   11.0 MB</text>
    <text x="428" y="136">0x0A80000  W_up     11.0 MB</text>
    <text x="428" y="164">0x1500000  W_down   11.0 MB</text>
  </g>
  <text x="580" y="136" fill="#a6885b" font-size="10">权重常驻</text>
  <text x="580" y="152" fill="#a6885b" font-size="10">fp4</text>

  <g stroke="#8aa8c4" fill="#f4f8fb">
    <rect x="420" y="184" width="150" height="22" rx="2"/>
    <rect x="420" y="210" width="150" height="22" rx="2"/>
    <rect x="420" y="236" width="150" height="22" rx="2"/>
    <rect x="420" y="262" width="150" height="22" rx="2"/>
    <rect x="420" y="288" width="150" height="22" rx="2"/>
  </g>
  <g font-family="monospace" font-size="9.5" fill="#345">
    <text x="428" y="199">0x1F80000  X  71x7168</text>
    <text x="428" y="225">0x2078800  G  71x3072</text>
    <text x="428" y="251">0x20E3000  U  71x3072</text>
    <text x="428" y="277">0x214D800  A  71x3072</text>
    <text x="428" y="303">0x21B8000  Y  71x7168</text>
  </g>
  <text x="580" y="225" fill="#5b7fa6" font-size="10">激活区</text>
  <text x="580" y="241" fill="#5b7fa6" font-size="10">bf16</text>

  <g stroke="#5b7fa6" fill="none" marker-end="url(#f5a)" stroke-width="1.2">
    <path d="M352,66 L416,192"/>
    <path d="M574,196 L610,196 L610,110 L646,110"/>
    <path d="M574,102 L600,102 L600,116 L646,116"/>
    <path d="M646,124 L610,124 L610,220 L574,220"/>
    <path d="M574,196 L594,196 L594,150 L646,144"/>
    <path d="M646,158 L604,158 L604,246 L574,246"/>
    <path d="M574,222 L640,222 L640,268 L574,270"/>
    <path d="M574,272 L620,272 L620,300 L574,298"/>
  </g>
  <g fill="#5b7fa6" font-size="9.5">
    <text x="650" y="104">Gemm gate</text>
    <text x="650" y="150">Gemm up</text>
    <text x="600" y="262">Elemwise swiglu</text>
    <text x="628" y="292">Gemm down</text>
  </g>
  <text x="360" y="182" fill="#5b7fa6" font-size="10">Recv</text>

  <path d="M574,300 L740,300 L740,180 L790,180" stroke="#5ba677" fill="none" marker-end="url(#f5a)" stroke-width="1.4"/>
  <text x="742" y="318" fill="#5ba677" font-size="10">Send</text>

  <rect x="780" y="40" width="100" height="290" rx="3" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="830" y="62" text-anchor="middle" fill="#432" font-weight="bold" font-size="11">外围模块</text>
  <text x="830" y="86" text-anchor="middle" fill="#765" font-size="10">收 384 份</text>
  <text x="830" y="104" text-anchor="middle" fill="#765" font-size="10">按 token 相加</text>
  <text x="830" y="128" text-anchor="middle" fill="#765" font-size="10">加 shared</text>
  <text x="830" y="146" text-anchor="middle" fill="#765" font-size="10">expert 的那份</text>
  <text x="830" y="204" text-anchor="middle" fill="#876" font-size="10">残差相加</text>
  <text x="830" y="222" text-anchor="middle" fill="#876" font-size="10">进下一层</text>
  <text x="830" y="246" text-anchor="middle" fill="#876" font-size="10">的 attention</text>

  <text x="450" y="376" fill="#333" font-size="12">一个核一层收发的量</text>
  <g font-size="11" fill="#555">
    <text x="60" y="400">进  71 x 7168 x 2 = 1,017,856 字节</text>
    <text x="60" y="420">出  同样大小，因为 down 把维度打回 hidden</text>
    <text x="60" y="440">61 层合计  一个核收发 1.14 亿字节，按 32 字节每拍的链路要 358 万拍</text>
    <text x="60" y="460">同一个核 61 层的计算  205 万拍</text>
  </g>
  <rect x="55" y="474" width="600" height="26" rx="3" fill="#fdf2f2" stroke="#c88"/>
  <text x="68" y="492" fill="#844" font-size="11">按这个排法通信是计算的 1.8 倍，瓶颈在搬运不在算</text>
</svg>
```

***

## 5. 时间

### 5.1 一层的时间轴

```svg
<svg viewBox="0 0 900 400" xmlns="http://www.w3.org/2000/svg" font-family="sans-serif" font-size="12">
  <text x="450" y="24" text-anchor="middle" fill="#333" font-size="13">一层的时间轴   按 32 字节每拍的链路、1024 字节每拍的权重加载算</text>

  <line x1="70" y1="330" x2="870" y2="330" stroke="#bbb"/>
  <g stroke="#ddd">
    <line x1="70" y1="60" x2="70" y2="330"/>
    <line x1="266" y1="60" x2="266" y2="330"/>
    <line x1="332" y1="60" x2="332" y2="330"/>
    <line x1="398" y1="60" x2="398" y2="330"/>
    <line x1="470" y1="60" x2="470" y2="330"/>
    <line x1="666" y1="60" x2="666" y2="330"/>
  </g>
  <g fill="#999" font-size="9.5" text-anchor="middle">
    <text x="70" y="346">0</text>
    <text x="266" y="346">31808</text>
    <text x="332" y="346">42560</text>
    <text x="398" y="346">53312</text>
    <text x="470" y="346">64916</text>
    <text x="666" y="346">96724</text>
  </g>
  <text x="470" y="366" text-anchor="middle" fill="#888" font-size="10">拍</text>

  <text x="60" y="80" text-anchor="end" fill="#432" font-size="11">外围模块</text>
  <rect x="20" y="66" width="46" height="20" rx="2" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="43" y="80" text-anchor="middle" fill="#765" font-size="9">attention</text>

  <g font-size="11" fill="#243" text-anchor="end">
    <text x="60" y="128">core 0</text>
    <text x="60" y="176">core 1</text>
    <text x="60" y="224">core 2</text>
    <text x="60" y="290">core 383</text>
  </g>

  <g stroke="#5ba677" fill="#dff0e6">
    <rect x="70" y="112" width="196" height="22" rx="2"/>
    <rect x="70" y="160" width="177" height="22" rx="2"/>
    <rect x="70" y="208" width="185" height="22" rx="2"/>
    <rect x="70" y="274" width="193" height="22" rx="2"/>
  </g>
  <text x="168" y="127" text-anchor="middle" fill="#243" font-size="10">Recv  31808 拍</text>
  <text x="158" y="175" text-anchor="middle" fill="#243" font-size="10">Recv  28672</text>
  <text x="162" y="223" text-anchor="middle" fill="#243" font-size="10">Recv  30016</text>
  <text x="166" y="289" text-anchor="middle" fill="#243" font-size="10">Recv  31360</text>

  <g stroke="#5b7fa6" fill="#dde9f4">
    <rect x="266" y="112" width="66" height="22" rx="2"/>
    <rect x="332" y="112" width="66" height="22" rx="2"/>
    <rect x="404" y="112" width="66" height="22" rx="2"/>
    <rect x="247" y="160" width="66" height="22" rx="2"/>
    <rect x="313" y="160" width="66" height="22" rx="2"/>
    <rect x="384" y="160" width="66" height="22" rx="2"/>
    <rect x="255" y="208" width="66" height="22" rx="2"/>
    <rect x="321" y="208" width="66" height="22" rx="2"/>
    <rect x="392" y="208" width="66" height="22" rx="2"/>
    <rect x="263" y="274" width="66" height="22" rx="2"/>
    <rect x="329" y="274" width="66" height="22" rx="2"/>
    <rect x="400" y="274" width="66" height="22" rx="2"/>
  </g>
  <g fill="#234" font-size="9.5" text-anchor="middle">
    <text x="299" y="127">gate</text><text x="365" y="127">up</text><text x="437" y="127">down</text>
    <text x="280" y="175">gate</text><text x="346" y="175">up</text><text x="417" y="175">down</text>
    <text x="288" y="223">gate</text><text x="354" y="223">up</text><text x="425" y="223">down</text>
    <text x="296" y="289">gate</text><text x="362" y="289">up</text><text x="433" y="289">down</text>
  </g>
  <text x="299" y="106" text-anchor="middle" fill="#5b7fa6" font-size="9.5">Gemm 10752 拍</text>

  <g stroke="#a6885b" fill="#f6ede0">
    <rect x="398" y="112" width="6" height="22"/>
    <rect x="379" y="160" width="5" height="22"/>
    <rect x="387" y="208" width="5" height="22"/>
    <rect x="395" y="274" width="5" height="22"/>
  </g>
  <text x="401" y="104" text-anchor="middle" fill="#a6885b" font-size="9">swiglu 852</text>

  <g stroke="#5ba677" fill="#dff0e6">
    <rect x="470" y="112" width="196" height="22" rx="2"/>
    <rect x="450" y="160" width="177" height="22" rx="2"/>
    <rect x="458" y="208" width="185" height="22" rx="2"/>
    <rect x="466" y="274" width="193" height="22" rx="2"/>
  </g>
  <text x="568" y="127" text-anchor="middle" fill="#243" font-size="10">Send  31808 拍</text>
  <text x="538" y="175" text-anchor="middle" fill="#243" font-size="10">Send</text>
  <text x="550" y="223" text-anchor="middle" fill="#243" font-size="10">Send</text>
  <text x="562" y="289" text-anchor="middle" fill="#243" font-size="10">Send</text>

  <text x="284" y="252" text-anchor="middle" fill="#aaa" font-size="14">. . .</text>

  <rect x="666" y="66" width="60" height="20" rx="2" fill="#f7f2ea" stroke="#a6885b"/>
  <text x="696" y="80" text-anchor="middle" fill="#765" font-size="9">下一层</text>
  <line x1="666" y1="86" x2="666" y2="330" stroke="#a6885b" stroke-dasharray="3,3"/>

  <rect x="70" y="60" width="196" height="4" fill="#c88"/>
  <text x="168" y="56" text-anchor="middle" fill="#844" font-size="10">收发这两段占一层的 66%</text>
  <rect x="470" y="60" width="196" height="4" fill="#c88"/>
</svg>
```

各核的长短不一样，因为路由分给它们的 token 数不同，46 到 77 之间浮动。Gemm 的长度只
跟权重有关，所以三段一样长；收发两段跟 token 数成正比。

### 5.2 时间从哪来

模拟器算出来的：包在 Router 里排队多久、按带宽切片要几拍、跨 chip 的线延迟、出口被
别人占着要等多久、credit 不够卡多久、屏障等对端的包多久、访存在仲裁器上排队多久。

kernel 里带的：`Gemm` 与 `Elemwise` 的 `cycles`。有实测值用实测值，没有就按权重加载
量估，一次要把 `k x n` 的权重全读一遍，除以每拍能读多少字节。

实测值不准只影响绝对值。通信路径上堵在哪、哪条链路是瓶颈、各核负载均不均，这些结论
不受它影响。

***

## 6. 跑起来

### 6.1 模拟器的组成

`src/bach/ksim/` 五个头文件加一个命令行工具，不动现有那套 bach 建模：

| 文件 | 管什么 |
| --- | --- |
| `kernel.h` `loader.h` | `.bachk` 的数据结构与读入 |
| `topology.h` | 物理连接与选路 |
| `router.h` | 每核一个，出口共用，按字节数占用 |
| `core.h` | 顺序执行 kernel，Recv 收不齐就停着等 |
| `array.h` | 片内按行列接线，片间靠四个角接 PCIe |
| `feeder.h` | 外围模块，照时序表喂与收 |
| `system.h` | 装配，事件驱动推进 |
| `tools/ksim_run.cpp` | 命令行入口 |

不逐拍推进。走完一步就问各部件下一次有事发生在什么时候，直接跳过去。一条 Gemm 要一
万多拍，那期间整个阵列没有状态会变，逐拍空转的话一次前向要跑几千万步；跳过去之后
DeepSeek-V4 全部 61 层跑一遍是 6.5 秒，逐拍要 532 秒。

两种推进方式在同一份输入上跑出来的 end_time、计算拍数、等待拍数、收发字节数逐位相同，
所以跳过去的那些拍确实没有状态会变。

```
ksim_run --in deepseek.bachk [--chips 16x4] [--cores 2x4] [--bw N] [--pcie-bw N]
```

### 6.2 建模的选择

这几处是照着物理来的，跟现有那套 bach 建模不同：

**每条链路各占各的。** 左邻、右邻、垂直邻居、PCIe 是四条独立的双向链路，往左发不挡着
往右发，堵只堵在同一条链路上。现有 bach 的 Router 是十二个端口抢一个出口。

**包不在节点上落地。** 包头到了就往下一跳转，走几跳只多几段线延迟，不是每跳重花一遍
传输时间。只有终点等包尾。

**核里四条通道并行。** 矩阵核、向量核、搬运的出方向与入方向各走各的，这一层把结果推
出去的同时下一层的输入可以正在收。指令顺序发射，发得出去要通道空着且要读的那几块都
写好了，依赖按 kernel 里的地址认。

**Send 占住传输时间。** 逐拍把数据推到链路上，占一次 setup 加按带宽算的传输时间，推
完就算完，不等对端收到。

**西边进，东边出。** 阵列两侧各有一个 PCIe Switch，进和出各接一边：喂进去的包落在目标
核那一行最左边那个 chip 的左出口上，结果一路往东，走到最右那一列的右出口才出得去。代价
是去程和回程都朝东走，两个方向的流量压在同一批东向链路上；进出都走最左那一列的话去程往
东、回程往西，各占各的，那样片内更松，但两个 Switch 只用得上一个。

**送之前先扣额度。** 外围模块往一个核送一份输入就扣它一份额度，那个核把这一层做完、
结果推出去了才还一份。容量是这个核同时装得下几份输入，是机器参数。扣光了时序表说该喂
也喂不进去，所以表排得比阵列跑得快时不会一路灌爆，堵会顶回到外围模块。一个核没额度不
挡着别的核，待发的按核分开排。

**激活区两层交替用一块。** 一层的数据在下一层还被读着，所以留两块；隔了两层的那一层
早做完了。一个核的地址占权重 33 MB 加两块激活共 39.6 MB，不随层数涨。

额度记在核头上，不记在某一条边上：还的时候不看这一包发去了哪。核只从外围模块收输入
时这样就够，核之间互相送的那天再按边记。

已知的简化，写在这里免得被当成结论：

- 包不切片。一个大包整块占住出口，同一条链路上两个包不交错。总带宽是对的，单个流等
  多久不对。
- 依赖只查读等写。写等读没查，靠激活区双缓冲避开。
- Recv 按到达顺序认包，不看是谁发来的。每层只从外围模块收一份，够用。
- 不建模 SRAM 容量。地址排得出来就算排得下。

### 6.3 跑出来是什么

DeepSeek-V4，384 个核各一个专家，61 层走一遍：

| 链路带宽 | 端到端 |
| --- | --- |
| 核间 32、PCIe 32 字节每拍 | 5680 万拍 |
| 核间 128、PCIe 128 | 1435 万拍 |
| 核间 512、PCIe 512 | 733 万拍 |

同一份 kernel 里的计算总量是固定的 7.74 亿拍（384 个核相加，每核 201 万拍）。带宽从
32 提到 512，端到端缩到八分之一，最后停在 733 万拍上不再往下走，因为那时时序表本身
就排到 731 万拍，搬运已经不是瓶颈了。

单独提一个提不动：数据要先过 PCIe 进阵列、再在片内走多跳，两段都得够宽。

核里四条通道并行在这一组数上没有收益，因为依赖链是严格串行的：一层的结果要先回到外围
模块、算完 attention 才有下一层的输入，核没有别的活可以填进去。有多份输入排成流水时
才用得上。

额度在这一组数上几乎没影响，1 份是 5683.7 万拍、16 份是 5677.0 万拍，差在千分之一
以内：时序表按各层最长的那个核排，本来就不会喂过头。它防的是表排错的时候。把整张表的
时刻全改成第 0 拍、让外围模块不看节奏一次灌完：

| 额度 | 端到端 |
| --- | --- |
| 2 份 | 5701.8 万拍 |
| 64 份 | 6007.8 万拍 |
| 不限 | 6007.8 万拍 |

瓶颈在哪，把各条链路的占用按比例排一遍就看得出来。最忙的是每个 chip 右出口那个核（片内
号 3）的 PCIe 口，48 条全在 98.5% 以上，各转发 1952 个包、1.79 GB；片内那 480 条链路的
中位数只有 9%。

48 条各转发 1952 个包不是巧合，1952 就是 61 层乘 32 个核。一个 chip row 是 4 个 chip 共
32 个核，它们的输入要一路往东送到各自的 chip，结果也一路往东送出阵列，去程回程压在同一
批东向链路上，所以每一条上过的量都是整行 32 个核每层各 1 MB。32 MB 按 32 字节每拍是 100
万拍，61 层 6100 万拍，跟实测的 5680 万是一个数。所以端到端几乎完全由每行只有一条东向
链路这件事决定，跟核算得多快、片内怎么走都没多大关系。带宽提到 512 之后不再往下走，是
因为那时这条链路不再是瓶颈，剩下的是时序表自己那条线。

额度小的反而快一点，这一版拓扑下差 5.4%：一次灌进去把链路挤住了，按额度放行则是数据到
得刚好够核用。更值得注意的是额度 2 那一行跟按正常时序表跑出来的 5680.2 万拍只差 0.4%：
额度接管流控之后，时序表排得准不准就没那么要紧，只要不比阵列实际跑得慢就行。

## 7. 还没定的

- `Gemm` 的时间现在直接用 kernel 里的 `cycles`。让 MatrixCore 真的按 `a` `b` `c` 的
  字节数走一遍存储、时间由仲裁器和带宽算出来，会更接近真实执行，但 `cycles` 就不该
  出现在 kernel 里。
- 时序表的拍数按各层最长的那个核排，外围模块自己算 attention 那一段还是个占位数。
- 外围模块照表回放，不知道阵列堵没堵。上面那组数里核有大半时间停在 Recv 上，真实的
  系统会反压回去，这里不会。
