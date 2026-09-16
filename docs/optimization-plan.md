# 优化计划（基于 2026-09-13 逆向结果）

> **状态 2026-09-14**：P2-D ✅、P1-B ✅（标定报告
> docs/enroll-sim-calibration.txt，阈值 650 落地）、P1-A ✅（缓存生命周期
> 补全）、P2-C ✅（真机已验证 0.25s→0.52s 降档）。新增落地（master 未发布，
> 针对"解锁慢、偶发重按"）：verify/enroll 动作开始/手指事件后 15s 内 120ms 快速轮询
> （EGIS0575_ACTIVE_FRAME_DELAY_MS）、按压稳定窗 400→250ms
> （EGIS0575_FINGER_SETTLE_MS 可调）、验证模板回馈内存版（原 P3 研究项，
> 默认开，EGIS0575_VERIFY_FEEDBACK=0 关闭；细节见 comparison §6
> 差异节）、校准块损坏自动恢复（立即重读 → v2 模板内嵌副本 → 1Hz 耐心重读，
> 上限 7 次；2026-09-14/15 实测"会话后全零块"形态：固件校准 RAM 失效后
> 由固件后台秒~分钟级重建，97 软复位不能加速、驱动内 USB 端口复位
> 反而使轮询数分钟不收敛，见 protocol §8 第 3 条）。
> 待办：P1-B4 真机录入回归
> （`./scripts/test-enroll-verify.sh enroll` 后同点重按 3~4 次
> 验证拒绝）与打包后的 fprintd 全链路复测；回馈的 FAR 余量扩样本复核。

> 工作计划文档（非定案参考；已收录入 docs/README.md 双语索引）。
> 依据：docs/protocol.md §2/§4C/§7（CET300 命令集、Windows 会话实际序列、
> 校准持久化真相）、docs/windows-enrollment.md（Windows 录入机制）。
> 每项标注来源、价值、风险、验收标准。

## 优先级总览

| 编号 | 项目 | 价值 | 工作量 | 风险 |
|---|---|---|---|---|
| P1-B | 录入相似帧拒绝（HIGHLY_SIMILARITY 移植） | **高**（精度/覆盖） | 1 天含标定 | 中 |
| P1-A | 校准内存缓存 + 快路径 open | 中（鲁棒性为主） | 半天 | 中低 |
| P2-C | 静默期自适应轮询 | 中（失敏缓解） | 0.5 小时 | 低 |
| P2-D | SSM 状态名日志 | 低（可观测性） | 0.5 小时 | 无 |
| P3-* | 研究项（不进驱动） | — | — | — |

---

## P1-B 录入相似帧拒绝

**来源**：windows-enrollment.md §3/§6——Windows 的 UpdateEnrollment 对
每帧做 HIGHLY_SIMILARITY 判定（SIMILARITY_THRESHOLD 门），同位置重复
按压**不入池**，等效提示换位置。

**问题**：我们 12 帧模板库可以全部来自同一按压点（test-enroll-verify.sh
虽提示换位置但不强制），位置覆盖退化 → 偏心验证按压分数下降（真机
冒充余量仅 15 分的一部分原因）。

**实现**（改动集中在 `on_frame_accepted_enroll`）：
1. 新帧通过 Stage-2 质量门后、写入 `enroll_feats[enroll_stage]` 前，
   对已录各帧跑 `egis0575_m_score(new, pooled[i])`；任一分数 ≥
   `EGIS0575_ENROLL_SIM_THRESHOLD` → 拒绝入池。
2. 拒绝时：`enroll_stage` 不推进，`fpi_device_enroll_progress` 带
   `fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER)`（提示挪位置）。
3. **先标定后启用**：写 `scripts/calibrate-enroll-sim.py`，用 datasets/
   现有数据集统计两类分布——同人跨按压帧对分（下界）vs 同点重复按压
   帧对分（上界），取分离点。预期两分布有重叠区，阈值取跨按压分布的
   P90 保守值；`EGIS0575_ENROLL_SIM_THRESHOLD` env 可调，0 = 关闭。
4. **防卡死**（对应 Windows 的 MAX_ENROLL_TRY）：连续 4 帧被拒后本阶段
   放宽接受（接受并 fp_dbg 记录），保证录入总能完成。

**验收**：
- 标定脚本输出两分布报告（留档 docs/ 或 datasets 报告）
- 真机录入 12 帧，模板库两两分均值下降（覆盖更广的离线证据）
- 录入总按压次数 ≤ 16（12 + 最多 4 次拒绝）
- 真机 verify 4/4 通过（与当前基线持平或更好）

**风险**：阈值错标伤录入体验 → 用保守值 + env 逃生门 + 连续拒绝放宽。

## P1-A 校准内存缓存 + 快路径 open

**来源**：protocol.md §4C——Windows 从不跑标定链（PHASE_1/3/5 + 72 读），
每会话只用 8 条命令上传**主机侧缓存**的校准块；传感器上电自标定。

**诚实定位**：实测我们的完整 A 链只要 ~385ms（settle 150 + 校准链 176 +
上传/POST_CAL ~60），快路径延迟收益仅 150–200ms，用户无感。真实收益：
- **鲁棒性**：二次 open 跳过 72 读 → 消除"校准块损坏检测→重试"路径
  的再次暴露（§8.3 的损坏只在读取时发生）
- **总线流量**：少 ~38 包 + 一次 5356B 读，降低传感器负担
- 与已验证机制同型：claim 回收重传校准（20s 156 次零超时）证明
  "73 重传即恢复成像"，无需 97 复位——这正是 Windows 快路径的原理

**实现**：
1. `egis0575_finish_close` 保留 `self->calibration`（进程内缓存；
   注释"sensor state across close is unknown"的担忧已被 §4C 证据推翻）
2. `dev_open`：有缓存 → 新增 `SM_FAST_OPEN` 路径：AGC 寄存器写（61
   0a/0c/50 族，取自 POST_RESET 表）→ `73 14 ec` 上传缓存 → `60 40` 式
   确认轮询（复用现有 60 00 轮询）→ POST_CAL 头 → 预热帧
3. **失效条件**（回退完整 A 链）：传感器健康看门狗触发、超时恢复
   （timeout_recoveries）、校准块损坏标志、transport wedge 恢复后
4. 不做磁盘持久化（驱动写文件上游不可行——审查 A2 的教训；进程内
   缓存已获主要收益，fprintd 常驻进程生命周期内有效）

**验收**：probe.sh 连续两轮 open，第二轮日志无 PHASE_1/72 读；
enroll→verify 端到端回归通过；人为触发看门狗（EGIS0575_PGM 长会话）
后回退完整链并恢复。

## P2-C 静默期自适应轮询

**来源**：protocol.md §7——Windows 占空比 <0.1%（静默期零轮询）；
我们固定 230ms（~7%）是 §8.6 长会话失敏的诱因之一。fprintd 语义要求
报手指状态 → 不能照抄静默，但可以分档。

**实现**：无手指活动 ≥30s → 轮询间隔放宽到 500ms（占空比 ~3%）；
任何手指事件/动作开始 → 立即回 230ms。改动点：帧循环的 delay 常量
变动态值（一个 idle-since 时间戳 + 两档间隔）。

**验收**：30 分钟 soak（无指）日志统计两档分布；期间随时录入仍能在
~1s 内响应（500ms 档的手指响应延迟可接受）；失敏发生率对比历史。

## P2-D SSM 状态名日志

journal 里 `SM_STATES_NUM entering state N` 无状态名——`fpi_ssm_new`
的 state_name 回调没接。补一个状态名表（SM_* → 字符串），可观测性
收益，零风险，与上述任一项同车提交。

## P3 研究项（独立于驱动，不进生产代码）

- **`98 00` / `90 00` 命令真机探测**：DLL 的 fp_tz_secure_set_power_off
  路径疑似 `98 00`（纯写无响应）。用 tools/hwpoll.py 式独立脚本 A/B
  探测（先 pcap 复核调用语境），若确认是下电命令 → 评估长静默期省电
  （与 P2-C 组合）。风险：未知命令可能挂死传输 → 只在可 reset-sensor
  的实验环境做
- **验证期模板回馈**（Windows 'AE' blob，169959B）：~~涉及模板变异 +
  上游不可行 → 仅作研究记录~~ **已实现进程内内存版**（2026-09-14，
  master 未发布）：高置信匹配帧回授内存模板库（每指纹 16 帧上限，
  同点去重，FIFO 只淘汰回授帧），随 fprintd 进程生命周期有效；磁盘
  持久化与上游化仍不可行（同校准缓存的论证），fprintd 常驻
  （`fprintd -t`）可跨会话保留。真机回归后评估 FRR 收益
- **校准块布局逆向**（Zone1/Zone2 坏点表 + VDM 参数）：对驱动无直接
  收益（透明 blob 已够用），仅当未来需要跨机型校准迁移时再投入

## 明确不做（及理由）

| 项 | 理由 |
|---|---|
| 变长录入（MIN/MAX_ENROLL_COUNT） | libfprint `nr_enroll_stages` 为类初始化静态值；现状"坏帧不推进"已近似，上游不可行 |
| 校准磁盘持久化 | 驱动写文件上游不可行（审查 A2 教训）；进程内缓存已获主要收益 |
| 中断端点 finger-down 通知 | Windows 实测未用（§7），固件能力存疑（EGIS_WAIT_INTERRUPT 字符串可能属非 USB 形态） |
| 复刻 Windows 静默期零轮询 | fprintd 语义要求持续报手指状态 |
| `60 01` 硬件在检 | 真机实测对按压无反应（§6，1199 次恒 0x01） |

## 实施顺序与回归矩阵

1. **P2-D**（半小时，热身 + 消除日志噪声）
2. **P1-B 标定脚本 → 驱动实现 → 真机录入/验证回归**（核心项）
3. **P1-A 快路径 → probe 双轮 + 看门狗回退验证**
4. **P2-C 自适应轮询 → 30 分钟 soak**

每步完成后回归矩阵：`bash -n` 全脚本、ninja 零警告、probe.sh 20s 健康
三指标（预热 3 帧/零超时/干净取消）、enroll→verify 4 连测、（P1-A/C 后）
30 分钟 soak。全部完成后按 v0.2.1 打 tag 发版（模板格式变更 + 本轮
健壮性修复 + 优化一起进包）。
