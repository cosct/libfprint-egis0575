# 文档索引 / Documentation Index

驱动随 [cosct/libfprint-egis0575](https://github.com/cosct/libfprint-egis0575) 仓库分发
（`libfprint/` 子树；AUR 同名包）。研究结论全部真机验证或源码可溯，
开放项集中在 [comparison §9](comparison.md#9-遗留问题) 与
[protocol §10](protocol.md#10-尚未逆向的部分)。

The driver ships from
[cosct/libfprint-egis0575](https://github.com/cosct/libfprint-egis0575)
(the `libfprint/` subtree; same-named AUR package). Every conclusion is
hardware-verified or source-traceable; open items live in
[comparison §9](comparison.en.md#9-open-items) and
[protocol §10](protocol.en.md#10-not-yet-reverse-engineered).

## 板块一：逆向研究 / Track 1: Reverse engineering

「传感器到底怎么工作、Windows 是怎么驱动它的」

| 文档 / Document | 一句话 / In one line | 中文 | English |
|---|---|---|---|
| USB 通信协议 / USB wire protocol | CET300 命令集全表、初始化/采集序列、Windows 会话实际序列与已知陷阱 / The full CET300 command set, init & capture sequences, the actual Windows session order, known pitfalls | [protocol.md](protocol.md) | [protocol.en.md](protocol.en.md) |
| 引擎系数提取 / Coefficient extraction | 从 vendor DLL 定位并提取 FIR/权重表的完整方法 / Locating and extracting the FIR/weight tables from the vendor DLL | [windows-engine-tables.md](windows-engine-tables.md) | [windows-engine-tables.en.md](windows-engine-tables.en.md) |
| Windows 录入机制 / Enrollment mechanics | WBF 引擎适配器的采样数三层覆盖、逐帧三道门、20 个可调参数 / The WBF engine adapter's layered sample counts, per-frame gating, 20 tunables | [windows-enrollment.md](windows-enrollment.md) | [windows-enrollment.en.md](windows-enrollment.en.md) |

## 板块二：驱动工程 / Track 2: Driver engineering

「我们据此怎么造的驱动、为什么这样造」

| 文档 / Document | 一句话 / In one line | 中文 | English |
|---|---|---|---|
| 架构对比与决策 / Comparison & decisions | 四实现对比、七个匹配方案失败史、Windows 引擎移植终局、稳定性工程 / Four-implementation comparison, the seven failed matchers, the endgame port, stability work | [comparison.md](comparison.md) | [comparison.en.md](comparison.en.md) |
| 优化计划 / Optimization plan | 逆向结论 → 优化项的路线图与完成状态（工作文档）/ The reverse-engineering-to-optimization roadmap with status (working doc, Chinese) | [optimization-plan.md](optimization-plan.md) | — |
| 阈值标定报告 / Threshold calibration | 录入相似拒绝阈值的数据分布证据 / The corpus distributions behind the enrollment similarity threshold | [enroll-sim-calibration.txt](enroll-sim-calibration.txt) | — |
| 回馈 FAR 复核报告 / Feedback FAR recheck | 验证模板回馈的离线回放复核与灰区运行定位 / Offline replay recheck of verify-time template feedback, gray-zone runs identified | [verify-feedback-eval.txt](verify-feedback-eval.txt) | — |

## 阅读顺序建议 / Suggested reading order

- **想懂驱动为什么长这样** / to understand the driver:
  protocol → comparison → windows-engine-tables
- **想改/调优** / to hack or tune:
  optimization-plan → comparison §6–§7 → 本仓库 README 的 env vars 一节
- **只想装来用** / just installing: 仓库 README 的 Install 一节即可 / the Install section of the repo README is enough
