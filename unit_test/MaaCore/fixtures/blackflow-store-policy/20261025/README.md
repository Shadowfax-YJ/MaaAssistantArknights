# 整局沙盘β购买策略

`producer-policies.json` 的两个对象由真实 `BlackFlowSession::run_log_state` 导出，
原生回放仅固定抽签结果，分别覆盖沿用默认规则和配额覆盖为 0。
不是实际用户对局，不修改历史包。

抽签在采集策略新局初始化时使用 `std::bernoulli_distribution(0.5)`，整局固定；
商店进入、刷新、扫描及树洞会话复制均不抽签。默认分支的 `quota_override=null`
表示沿用现有规则；缺少历史字段表示未知，不能解释为默认分支或“没有沙盘β”。

回放覆盖两分支的实际商店选择、仅剩被禁购商品时正常结束选择、其他物品配额保持、
多次商店重置不变和新局初始化。策略不限制非购买途径，也不丢弃已获得的沙盘β。
同一样本同步至 analysis `tests/fixtures/blackflow-store-policies.json`，验证两分支
均兼容独立签名归档；消费者不据策略标记新增/删除物品事实，未来 schema 仍拒绝。
