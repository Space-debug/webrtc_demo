# UDP/ICE 问题分析

## 1. iptables 结论：**未拦截 UDP**

```
Chain INPUT (policy ACCEPT)   ← 默认放行
Chain OUTPUT (policy ACCEPT)  ← 默认放行
```

- 默认策略为 ACCEPT，未显式 DROP/REJECT UDP
- 唯一自定义规则：`ACCEPT tcp dpt:4000`，仅放行 TCP 4000，不影响 UDP

**结论：iptables 不是原因。**

---

## 2. 网络拓扑

| 接口 | IP | 用途 |
|------|-----|------|
| lo | 127.0.0.1 | 回环 |
| eth0 | 192.168.3.222 | 有线网卡 |

- 只有 eth0 和 lo，**无多网卡冲突**
- 192.168.3.222 在 eth0 上

---

## 3. ICE 选 192.168.3.222 的影响（已诊断）

**根因**：本机推拉流时，ICE 选 192.168.3.222，发往本机 IP 的 UDP 包在默认路由下**未正确走自环**，导致媒体包无法送达。

**验证**：添加 `ip route add 192.168.3.222/32 dev lo` 后，UDP 媒体正常，20 秒内收到 270 帧。

**关键**：路由必须在**推流启动前**已存在。若路由缺失，ICE 会显示 Connected 但 RTP 无帧。

---

## 4. 手动操作

```bash
# 添加（本机推拉流前，必须）
sudo ip route add 192.168.3.222/32 dev lo

# 删除（可选，跨机推拉流时无需此路由）
sudo ip route del 192.168.3.222/32 dev lo
```

## 5. 诊断脚本

若拉不到帧，运行 `./scripts/diagnose_udp.sh` 会检查路由、启动推拉流并输出结果。

---

## 6. 跨机推拉流

跨机时无需此路由，ICE 会选对端 IP，UDP 媒体经 eth0 正常传输。
