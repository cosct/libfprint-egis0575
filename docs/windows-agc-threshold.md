# Windows AGC 闭环与验证阈值自适应逆向笔记（v3.7.1.1 复核）

> **状态：静态分析定案 + 移植落地（2026-10-04）。** 两版本交叉验证
> （acerdrv v3.7.1.1 Ghidra 反编译 + 2021 Animeshz 档案逐指令对照），
> 核心算法双版本同构；标"推断"处为反编译语义解读，未逐指令验证。
> §2.4 AGC 闭环与 §4.1 二次机会已随驱动实现并真机验证（本机）；
> §3.3 自适应阈值论证为不可移植。本文是协议/引擎文档的补篇，
> 不改变任何已定案结论。
> [English version](windows-agc-threshold.en.md)

## 1. 方法与材料

- **v3.7.1.1 反编译导出**：Ghidra 12.1.2 headless，`-import` + postScript
  `scripts/GhidraDumpAll.java`（导出全部函数反编译 + 字符串表）。
  EgisTouchFP0575.dll 702 函数、Engine 635 函数、Sensor 260 函数。
- **主 DLL 自动分析欠完整**（老问题：v3.7.1.1 的代码流让线性反汇编失步，
  见 windows-engine-tables §1 的方法论记录）。关键函数用锚字符串的
  rip-relative 引用扫描定位后 capstone 定向反汇编，锚点：
  `et5xx_fetch_dynamic_intensity`（RVA 0x3dbd0）、`fp_tz_secure_set_power_off`
  （0x3d978）、`et5xx_power_on_initialize`（0x3da80）。
- **交叉参照**：`refs/EgisTec-EH575/findings/EgisTec-EH575/decompiled_source/ghidra/`
  的 2021 版反编译（主 DLL 39034 行）。两版本函数逐指令对照后确认同构。
- 产物（/tmp，可按上述命令再生）不随仓库分发；本文记录全部 RVA 与常量，
  独立可复核。

## 2. et5xx_fetch_dynamic_intensity：逐帧 AGC 强度闭环（首要移植候选）

### 2.1 证据

| 项 | 2021 版 | v3.7.1.1 |
|---|---|---|
| 函数 | `FUN_180009b20`（egis_fp_common_5XX.c） | RVA 0x9490/0x9530 区（日志行 0x4d4/0x4e5 = 1236/1253 行） |
| 强度表 | 栈上立即数 16 项 | .rdata RVA 0x3dc10（文件偏移 0x3d010），16 项，后邻字符串 `et_get_image_state` 定界 |
| 调用点 | `egis_fp_get_image.c` 取帧循环内（每帧一次，多处调用） | 同（字符串/行号对应） |

16 级累计强度表（双版本一致；2021 栈转储的第 12/16 项乱码由此修正为
0x9800/0xbe66）：

```
0x0800 0x395c 0x42e1 0x4c66 0x508f 0x5f33 0x68cd 0x7266
0x7c00 0x859a 0x8e66 0x9800 0xa19a 0xab33 0xb4cd 0xbe66
```

v3.7.1.1 核心循环实摘（0x94c4–0x94f7，与 2021 完全对应）：

```
movzx  edi, byte [rbp-0x40]        ; 实测强度 b0
mov    cl, 0x12                    ; 寄存器 0x12（表行索引）
shl    edi, 0xa                    ; ×1024
call   read_reg                    ; → [rbp-0x41]
loop:  mov edx, [rbp + rax*4 - 0x21]  ; table[k]
       cmp edi, edx / jl exit
       sub edi, edx / dec ebx         ; 沿表降档
```

### 2.2 算法（伪代码，语义解读）

```
测量：写 62 2c 02 {00 57}（priming）→ 读 63 67 03 得 b0,b1,b2
      （另一路径：ctx[0x20]==1 时走 FUN_180008a40 的图像统计变体）

if (b0 == 0 && b1 == 0xff):            # 双端饱和
    读 60 12 → k；k > 1 则写 61 12 k-1  # 表行索引下移一档
else:
    if (ctx->level == 0xff): 读 60 0f → ctx->level   # 首次：采纳传感器现值
    if (b1 == 0xff):                     # 过亮 → 降强度档
        x = b0 << 10; 读 60 12 → k
        while (x >= table[k] && ...): x -= table[k]; level--
    elif (b0 == 0):                     # 过暗 → 升强度档（cap 0x40）
        x = (0xff - b1) << 10; 读 60 12 → k
        while (x >= table[k] && level < 0x40): x -= table[k]; level++
    if (level != ctx->level): 写 61 0f level; ctx->level = level
```

寄存器语义（行为级，推断）：`0x0f` = 强度/增益档位（写）；`0x12` = 当前
表行索引（读写）；`0x2c/0x2d` = 测量 priming；`0x67` = 3 字节强度读数
（读）。**`71 45 06 00 XX 87 13 00 03` 第 5 字节 XX 即此闭环维护的强度值**
（protocol §2 原记"动态强度值"，实测同机型跨会话 0xb9/0xbe 的来源）。

### 2.3 与本驱动的对照

| Windows | 本驱动现状 |
|---|---|
| 每帧闭环调节 0x0f/0x12，`71` 包字节动态 | `71` 包字节硬编码 0xb9（egis0575.h SHUTDOWN_PACKETS 末包）；AGC 寄存器 61 0a/0b/0c 全静态表值 |
| 曝光漂移被持续纠正 | 漂移无人管 → 失敏后靠看门狗重跑全链恢复（秒级） |

**失敏的机制性解释（假说，与 protocol §8.6 现象吻合）**：>10 分钟轮询的
coverage 50%→2–5% 衰减，正是无闭环时曝光/强度漂移的预期表现；Windows
占空比 <0.1% 且每帧 AGC，两个机制叠加使其从不暴露此问题。

### 2.4 移植实施（2026-10-04 已落地，EGIS0575_AGC=1 启用、默认关）

实现比原草案更省：遥测不需要新 priming——采集表（POST_CAL/POST_INIT）
里本来就有测量三元组 `63 2c 02 00 57` → `60 2d` → `62 67 03`（10 字节
响应，data 在 resp[7..9]）和 `60 0f 03`（resp[5] = 当前强度档位），
驱动此前一直丢弃这些响应。移植后：

- resp_cb 解析遥测（b0/b1/level），rail 条件判定与 DLL 分支结构一致
  （b0==0 且 b1==0xff → `60 12` 读行后 `61 12 k-1`；仅 b1==0xff → 走表
  降档；仅 b0==0 → 走表升档 cap 0x40）；调整经 SM_AGC_* 四状态在
  SM_INIT 前插入，错误一律降级跳过（不失败动作）；
- **健康传感器零额外流量**：遥测搭既有包便车，rail 不触发就无任何
  新传输——与 DLL 行为同型；
- 真机验证（2026-10-04，本机 20s 探测 + 15 分钟 soak）：遥测稳定
  mid-range（b0≈0x7e–0x80、b1≈0x85–0x88、level=0x20），全程零调整
  写入、零超时、采集循环健康（预热 3 帧 + 空闲轮询正常）；
- **71 包字节溯源未定**：实机 b0≈0x7f 与 pcap 实证的 0xb9/0xbe 不符，
  动态写入拆成独立开关 `EGIS0575_AGC_71=1`（默认关，静态 0xb9 是
  已验证值）；
- 待办：rail 场景的真机复现（人为遮挡/降温？）与 30 分钟以上 soak
  的失敏对比，之后再评估默认开启。

已知陷阱（实现期踩过）：SM_INIT 正常路径尾部原用 `fpi_ssm_next_state`
依赖状态相邻性直达 SM_START——在两者之间插入 SM_AGC_* 后会落入 AGC
链形成死循环（表现为每毫秒一轮 `61 0f` 写入）。已改为显式
`fpi_ssm_jump_to_state (SM_START)`；后续在该区域插状态时同样注意。

## 3. 验证阈值自适应（'AE' 机制——防锤击滞回，当前标定下不可移植）

### 3.1 算法（v3.7.1.1 `FUN_1800067c0` ≡ 2021 `FUN_180006250`，逐行同构）

model-5 参数（见 §5 表）：基础阈值 `[0x15]` = 660，`[0x21]` = 735，
票数门 `[0x1a]` = 85。

```
score = 核心匹配器输出（FUN_18000bad0）
if (score > base):                        # 轴是 base(660)，不是 effective
    effective -= 80；floor = 735          # 只退回起点，不会更低
else:
    effective += 60；cap = base + base/2 = 990
# 之后用【更新后的 effective】参与本次的判定
if (score > effective && ctx[2] < 85):
    → 二次机会救援路径（§4.1），成功则置匹配标志
```

**方向性要点（2026-10-04 复核修正）**：effective 的区间是 [735, 990]，
**起点就是下限 735 > base 660**——即 Windows 的 day-one 判定门槛比其
标定基线还严；−80 只是把失败累积的收紧退回起点，从不会降到 735 以下。
该机制的实质是**防锤击滞回**（反复失败 → 门槛上探 990，高分 → 回落
735），不是"越用越宽松"的 FRR 改善。佐证：992xxx 调用方在记录切换时
直接把 effective 重置回 735（静息点）；真机 trace 的成功验证分数 930
恰落于 [735, 990] 带内。

判定链的"干净通过"分支（score > effective 且 ctx[2] 足票）在管线虚表
之后（EngineAdapterVerifyFeatureSet → pipeline 对象），未逐指令定位；
ctx[2] 为匹配上下文的第二指标（推断为 Σ ceil((128−h)/16) 量级的加权
票数），85 至少是救援路径的门。此开放点不影响 §3.3 的移植结论——
结论只依赖走步动力学。

### 3.2 'AE' blob 持久化

自适应状态（基础/有效阈值等 short 字段簇，全局单例）序列化进加密注册表
blob（头部 "AE"/0x4541，密钥 DAT_180038248，真机 trace 实测 169959 字节，
EngineAdapter.txt 的 `Update(commit, size=169959)`）。无 blob 时回落型号
默认。本驱动的进程内 feedback cache（comparison §6）是其同型减配。

### 3.3 移植评估：当前标定下不可移植（结论：不做）

忠实转写要求 floor > pivot 才非平凡（Windows 735 > 660）。在我们的
分数标度上：

- **floor = pivot = 335**：走步是空操作——逐 probe 语义下，score > 335
  的 probe 先把 effective 拉回 floor 再判定（等价于固定 335 直接过），
  score ≤ 335 的 probe 本就不可能过。与现行判据完全等价；
- **floor > 335**（如按 Windows 比例 335×1.114 ≈ 372）：(335, floor]
  分数带的 probe 会被拒——而 2026-09-13 重标定的可信真手指最低分恰为
  **349**，落在此带内，FRR 直接劣化；
- **floor < 335**：拿 FAR 换 FRR，与标定（可信冒充峰值 288、灰区 321
  vs 阈值 335）的取舍方向相反。

Windows 机制依赖其分数结构的大余量（base 660 与判定门 735 之间有整档
空间）；我们的真手指余量仅 14 分，结构上不兼容。除非扩数据集把可信真
手指下界拉开，否则维持固定 335 + agree 双帧判据；FRR 侧的改进走 §4.1
的低票二次机会路径。

## 4. 次要保真项

1. **低票二次机会（2026-10-04 已实施，默认开）**：分数过线但票数
   不足时重匹配一次。Windows 用旋转补偿（`FUN_18000efd0`，参
   [0x16]=5）；本驱动对应物 = `probe_matches_gallery` 在 best ≥ 阈值
   而 agree 帧不足时，用加倍几何容差（12px/24°，
   `EGIS0575_M_SC_*`）重算 agree——best 分仍取自严格档，FAR 侧被
   不变阈值封顶。离线回放（tools/egis0575-feedback-eval，36 可评分
   运行）：**救回 1 个真手指运行**（verify-run-194308-5，best=403
   过线但 agree 不足，此前误判 NO-MATCH），**冒充侧零翻转**。
   `EGIS0575_SECOND_CHANCE=0` 关闭。
2. **票权重量化**：Windows 平移直方图票权 = `ceil((128−h)/16)`
   （v3.7.1.1 `FUN_180009b20`@7492），本驱动为等权计数 + Σ(128−h) 分离
   计分。影响小，保持现状即可。

## 5. model-5 引擎参数表全量提取（v3.7.1.1 `FUN_18000e4e0` 默认 + case 5 覆盖）

| 偏移(×4) | 默认 | model 5 | 语义（推断） |
|---|---|---|---|
| [0x00] | 16 | 16 | 录入目标帧数上限族（windows-enrollment §2 的型号默认层） |
| [0x01] | 1000 | 1000 | 质量分尺度 |
| [0x03] | 3 | 3 | — |
| [0x04] | 9 | 9 | — |
| [0x05] | 0x700 | 0x700 | — |
| [0x0f] | 0x288 (648) | 648 | 次级分数门 |
| [0x11] | 0x400 (1024) | 1024 | — |
| [0x12] | 0x73 (115) | 115 | **Hamming 预算**（= 本驱动 EGIS0575_M_HAMMING_BUDGET ✓） |
| [0x13]/[0x14] | 7 / 8 | 7 / 8 | 几何容差族（px/角度） |
| [0x15] | 660 | **0x294 (660)** | 验证基础阈值 |
| [0x16] | 5 | 5 | 最小一致对（= EGIS0575_M_MIN_MATCHED ✓） |
| [0x17] | −1 | −1 | — |
| [0x18] | 0x18 (24) | 24 | — |
| [0x1a] | 100 | **0x55 (85)** | 验证票数门 |
| [0x1b]–[0x1e] | 10/20/5/240 | 同 | 冗余/进度族（windows-enrollment §5 对应） |
| [0x20] | 20 | 20 | — |
| [0x21] | — | **0x2df (735)** | 有效阈值初始/下限 |
| [0x22] | 170000 | 170000 | 二次机会质量门（§4.1 的 0x88 偏移项） |
| [0x26] | 2 | 2 | — |

（仅列非零/有语义项；完整原始转储见反编译导出。）

## 6. 确认等价/无需移植（本次复核新增证据）

| Windows 实现 | 证据 | 结论 |
|---|---|---|
| `fp_tz_secure_set_power_off` | v3.7.1.1 RVA 0xa074（日志行 0x244）：读 0x40 判模式 → `61 0c 22 / 61 0b 03 / 61 0a fc`（+可选 `61 09 43`） | **不是深下电命令**，就是重挂收尾；与 SHUTDOWN_PACKETS 尾族一致。`98/90` 命令族维持 P3 不动 |
| 重挂前奏 | 0x9f4c（行 0x22e）：`61 0a f4 / 61 0c 44 / 61 40 00` + 0x40 轮询 | 与 CAL_PHASE_5/POST_RESET 值一致，无需改 |
| `DeviceIdleEnabled/DefaultIdleTimeout/ResumingDetectModeParameterTuningEnabled` | 2021 `FUN_180014280`（注册表读取，空闲等待超时 + 恢复重调检测参数） | 三档占空比轮询 + 重挂序列等价（protocol §7 结论不变） |
| 质量门 | v3.7.1.1 `FUN_18001c230`（AcceptSampleData）：iResult→RejectDetail 9/7、`count_image_type_identifier()≠3`→5 | 与 windows-enrollment §3 一致；其判别器（`FUN_180004410`，重采样+查表出 type/amp/score）为专用实现，Stage-2 门已真机验证，等价可选 |
| 传感器适配器 | EgisTouchFPSensor0575.dll 仅转发 SensorStatus（WINBIO_SENSOR_REJECT 一处） | 无新可移植项 |

## 7. 证据索引

| 内容 | 2021 版 | v3.7.1.1 |
|---|---|---|
| AGC 函数 | EgisTouchFP0575.c `FUN_180009b20`（L7586 起） | RVA 0x9490/0x9530（capstone 定向反汇编，§2.1 摘录） |
| AGC 调用点（取帧循环） | 同文件 L8516/8771/9045/9103 | 字符串+行号对应 |
| 强度表 | 同函数栈立即数（16 项） | .rdata 0x3dc10（文件 0x3d010） |
| 验证+自适应 | EgisTouchFPEngine0575.c `FUN_180006250`（L6228 起） | `FUN_1800067c0`（导出行 5116 起；−0x50/+0x3c @ 5162/5176） |
| 'AE' blob 解密/回写 | 同文件 L4970/5080 区 | 导出行 3823 区（0x4541） |
| 票权量化 | — | `FUN_180009b20`@L7492（engine 导出） |
| 二次机会 | `FUN_180006250` 尾部 | `FUN_1800067c0` 尾部（`FUN_18000efd0`） |
| 下电/重挂 | —（无此函数名） | 0xa074 / 0x9f4c |
| model-5 参数表 | `FUN_180012d20` 区 | `FUN_18000e4e0` + case 5 @15079 |

复核未发现任何与 protocol.md/windows-engine-tables.md 已定案条目冲突的
证据；四源交叉验证的协议基准维持不变。
