# Windows 端录入（Enrollment）机制研究

> 英文版：[windows-enrollment.en.md](windows-enrollment.en.md)
>
> 素材：acerdrv v3.7.1.1（EgisTouchFPEngine0575.dll 静态分析）交叉
> refs/EgisTec-EH575 的 2021 版 Ghidra 反编译与真机 EngineAdapter/SensorAdapter
> trace。所有结论标注证据来源；标注"推断"处为反编译语义解读，未逐指令验证。

## 1. 架构位置：WBF 引擎适配器

Windows 侧由 WinBio 框架（WinBio.dll）驱动三个适配器：Sensor（采集）、
Engine（特征提取与匹配）、Storage（模板库）。EgisTouchFPEngine0575.dll
只导出一个符号 `WbioQueryEngineInterface`，其余入口（含录入全家桶）通过
`WINBIO_ENGINE_INTERFACE` 结构体给出。录入相关入口：

| WBF 入口 | Egis 实现（CTouchSensor::） | 说明 |
|---|---|---|
| EngineAdapterCreateEnrollment | EngineAdapterCreateEnrollment | 建录入上下文、定目标帧数 |
| EngineAdapterAcceptSampleData | AcceptSampleData | 每帧独立提取 + 质量门（Purpose=4 为录入；2 为验证） |
| EngineAdapterUpdateEnrollment | EngineAdapterUpdateEnrollment | 相似/冗余判定，决定帧是否入池 |
| EngineAdapterGetEnrollmentStatus | （读上下文状态字段） | 完成度/是否还需采样 |
| EngineAdapterCommitEnrollment | （serialize + StorageAddRecord） | 生成最终模板入库 |
| EngineAdapterQueryExtendedEnrollmentStatus | — | 扩展进度查询 |

真机 trace（EngineAdapter.txt，2021 版 DLL）验证了验证路径的调用序：
`PushDataToEngine → AcceptSampleData（Purpose=2，Extract Finish, iResult=0,
quality）→ IdentifyFeatureSet → Verify: result=0,930 → GetEnrollmentFeature →
Update(commit, size=169959→169959)`——最后一步即文档已记录的**验证期模板
回馈**（加密注册表 blob 'AE'，169959 字节）。

## 2. 目标帧数：三层覆盖

CreateEnrollment 里采样数不是常量，而是三层配置的叠加：

1. **传感器型号默认**（代码内置）：按型号查询（虚调用 +0x98）设参数——
   型号 5 → 10 帧；型号 7/8 → 16 帧；其余 → 16 帧另一组策略位
   （参数 id 0x4bf/0x4bd/0x4cd/0x4b4，推断为 MIN_ENROLL_COUNT 与策略开关）。
2. **WinBio 注册表覆盖**：`HKLM\SOFTWARE\Microsoft\Windows\
   CurrentVersion\WinBio` 的 DWORD 值 `Minimum Fingerprint Samples`——
   仅接受 8..20，越界回落 16，随后按传感器模式 **−2**（普通）或 **−1**
   （另一模式）。字段名 `m_MinEnrollFingerCount`（退出日志打印）。
   该注册表值不存在时保持第 1 层默认。
3. **设备参数覆盖**：打开设备专属注册表键（虚调用 +0xa8 给出键名，
   即 Attach 日志中的 `EgisFP\FPParameters` 家族），枚举键值逐一匹配
   20 个 `ENROLL_CTX_*` 名字（SetEnrollConfig，命中即按 id 注入引擎）。

## 3. 逐帧质量检查（AcceptSampleData → UpdateEnrollment）

每按压一帧：

1. **独立特征提取**：`Extract Finish, iResult=%d, quality=%d`。
   iResult 非 0 → RejectDetail 9 或 7（WinBio BAD_CAPTURE，用户重试，
   不消耗名额）；iResult∈{−6,−3} → E_FAIL。
2. **图像类型检查**（仅活动控制台会话的 PBA/登录流程）：
   `count_image_type_identifier() != 3` → RejectDetail 5（BAD_CAPTURE）。
3. **UpdateEnrollment 判定**（引擎核心，返回码决定帧命运）：
   - `1 = IMAGE_OK`：特征入池，CurrentEnrollNum+1；
   - `3 = HIGHLY_SIMILARITY`：与池内已有帧过于相似（受
     `SIMILARITY_THRESHOLD` 控制）——**不入池但计数+1**，等效提示
     "换个位置再按"；
   - `4 = REDUNDANT_ACCEPT`：冗余帧策略（`REDUNDANT_*` 参数族）；
   - `2`：特殊路径，走合并/定稿（thunk 产生合并结果并记录尺寸）。
4. 调试模式下每帧可落 `Enroll_Skeleton_%02d` / `Enroll_Orininal_%02d`
   （骨架/原始特征转储，`EnrollmentCount=[%d], FSize=[%d]`）——
   说明**模板是逐帧特征的池化序列**（与本驱动 12 帧模板库同构），
   而非融合单模板。

## 4. 完成与提交

- **变长录入**：`MIN_ENROLL_COUNT`/`MAX_ENROLL_COUNT` 夹取有效帧数；
  `MAX_ENROLL_TRY` 限制总尝试；`ENROLL_PROGRESS_THRESHOLD` 驱动进度。
  GetEnrollmentStatus 直接读上下文的完成 HRESULT + RejectDetail。
- **CommitEnrollment**：录入上下文 serialize 出模板 blob →
  `WbioStorageAddRecord` 入库；入库前查容量（GetRecordCount vs 上限，
  满则 `WINBIO_E_DATABASE_FULL`）。
- 提交后清上下文（ClearEnrollment）。

## 5. ENROLL_CTX_* 可调参数全表（v3.7.1.1，RVA 0x2e6f0 起 20 个）

| 参数 | 推断语义 |
|---|---|
| ENROLL_POLICY | 总策略开关 |
| MAX_ENROLL_ROTATION | 帧间最大旋转容忍 |
| EXTRACT_SKELETON | 骨架提取模式 |
| MIN_MINUTIAE_COUNT | 每帧最少细点数（质量门） |
| MAX_CANDIDATE_COUNT | 候选帧上限 |
| MAX_ENROLL_TRY | 总尝试次数上限 |
| MIN_ENROLL_COUNT / MAX_ENROLL_COUNT | 有效帧数上下限（变长录入） |
| REDUNDANT_CHECK_START | 从第几帧起启用冗余检查 |
| REDUNDANT_CONTINUOUS_BOUND | 连续冗余判定界 |
| EXTRACT_THRESHOLD | 提取质量阈值 |
| SELECT_COUNT_THRESHOLD | 入选最终模板的帧数阈值 |
| GENERALIZATION_THRESHOLD | 模板泛化/合并阈值 |
| ENROLL_PROGRESS_THRESHOLD | 进度推进阈值 |
| SIMILARITY_THRESHOLD | HIGHLY_SIMILARITY 门 |
| DUPLICATE_THRESHOLD | 重复帧门 |
| REDUNDANT_INPUT_THRESHOLD / _TRYCOUNT_POLICY / _THRESHOLD / _IMAGE_POLICY | 冗余输入策略族 |

默认值在引擎核心构造中按传感器型号设置（见 §2 第 1 层），注册表逐项覆盖。

## 6. 与本驱动的对照与可移植项

| Windows 做法 | 本驱动现状 | 评估 |
|---|---|---|
| 目标帧数 10/16（型号默认+注册表覆盖） | 固定 12（`EGIS0575_ENROLL_FRAMES`） | 数量级一致，无需改 |
| 逐帧提取质量门（iResult + RejectDetail） | Stage-2 质量门（grain/ridge/minutiae） | 已有，语义等价 |
| **HIGHLY_SIMILARITY：同位置重复帧不入池** | 已移植（v0.2.1）：新帧对已录帧 `egis0575_m_score` ≥ 650 即拒绝入池、不推进阶段，连续拒绝 4 次后放行保底（`EGIS0575_ENROLL_SIM_THRESHOLD` 可调、0 关闭） | 完成：真机标定同人同按压 p10=1889 vs 跨按压最大 642，0% 误杀（`enroll-sim-calibration.txt`） |
| 变长录入（MIN/MAX_ENROLL_COUNT + MAX_ENROLL_TRY） | 固定 12 阶段 | libfprint 的 `nr_enroll_stages` 为类初始化时静态值，变长需改框架，上游不可行；12 阶段 + 坏帧不推进（现状）已近似 |
| 模板 = 逐帧特征池化 | 12 帧特征模板库 | 同构 ✓ |
| Commit 前存储容量检查 | fprintd 侧管理 | 不适用 |
| 验证期模板回馈（169959B 'AE' blob） | 已移植进程内内存版（master 默认开，`EGIS0575_VERIFY_FEEDBACK` 可关）：高置信匹配帧回授进内存模板库，随 fprintd 进程生命周期有效；加密磁盘持久化 blob 未移植 | 内存版已完成（2026-09-14）；跨会话保留可让 fprintd 常驻，见 comparison §6 |
| 注册表 20 参数可调面 | 驱动 env vars | 研究等价物已够用 |

**已移植（随 v0.2.1 发布）——录入相似帧拒绝**：`on_frame_accepted_enroll`
在特征并入模板库前，用现成的 `egis0575_m_score` 对新帧与已录各帧打分；
相似分 ≥ 650（真机标定阈值，`EGIS0575_ENROLL_SIM_THRESHOLD` 可调，见
`enroll-sim-calibration.txt`）时不入模板库、不推进阶段，通过
`fpi_device_enroll_progress` 报 `FP_DEVICE_RETRY_GENERAL` 重试错误提示
用户换位置；连续拒绝达 `EGIS0575_ENROLL_SIM_MAX_REJECTS`（4）次后放行，
保录入可完成（对标 Windows 的 MAX_ENROLL_TRY）。效果：12 帧模板库的
位置覆盖显著提升，直接对标 Windows 的 HIGHLY_SIMILARITY + REDUNDANT 族，
且不触碰 libfprint 框架约束。

## 7. 证据索引

- 导出表仅 1 个符号（WbioQueryEngineInterface）：v3.7.1.1 PE 导出目录。
- 三层采样数：2021 dump `EngineAdapterCreateEnrollment`（RegOpenKeyExA
  "SOFTWARE\\...\\WinBio" + "Minimum Fingerprint Samples"，8≤v<21 否则 16，
  −1/−2 调整；型号 5/7/8 分支设 0x4bf/0x4bd 参数；设备键 SetEnrollConfig）。
- 逐帧质量检查：dump `CTouchSensor::AcceptSampleData`（iResult→RejectDetail
  9/7/5、count_image_type_identifier==3、Purpose==4 录入分支、
  Enroll_Skeleton_%02d 转储）与 `EngineAdapterUpdateEnrollment`
  （IMAGE_OK/HIGHLY_SIMILARITY/REDUNDANT_ACCEPT 三态 + CurrentEnrollNum）。
- 20 参数名：v3.7.1.1 .rdata RVA 0x2e6f0–0x2e988 字符串簇。
- 验证调用序与回馈：`logs/EngineAdapter.txt` / `SensorAdapter.txt` 真机 trace。
