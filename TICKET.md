# IceDriver 卡密系统（Ed25519）

固定 key 换成服务器签发的**票据**；内核只验签，不联网。

## 结构

```
server/app.py            卡密服务器（标准库 + openssl 签名）
server/../keys/ed25519.pem   私钥（只在服务器）
kernel/ice_ticket.c      票据解析 + 过期检查
kernel/ed25519_verify.c  Ed25519 验签（TweetNaCl 精简）
client/ticket_client.py  换票参考实现
```

## 票据格式

```
ICE2.<设备哈希hex8>.<到期时间hex8>.<签名hex128>     共 151 字符
```

- 签名消息：`ICE2.<dev>.<exp>`
- 模块里**只有公钥**，私钥只在服务器；dump 模块也伪造不了票据
- 内核校验：格式、签名、是否过期
- 设备绑定、封禁、卡密有效期在服务器判断

## 生成密钥

```sh
openssl genpkey -algorithm ed25519 -out keys/ed25519.pem
openssl pkey -in keys/ed25519.pem -pubout -outform DER | tail -c 32 | xxd -p -c 32
# 把输出的 64 位 hex 填进 kernel/ice_ticket.c 的 ICE_PUBKEY
```

## 部署

```sh
cd server
ICE_ADMIN_TOKEN=<管理口令> ICE_KEY=../keys/ed25519.pem python3 app.py --port 8080
```

服务器需要 `openssl` 命令（3.0+）。

## 管理卡密

```sh
T=<管理口令>
curl -X POST -H "X-Admin-Token: $T" -d '{"card":"VIP-0001","expire":"2027-06-30"}' http://host:8080/admin/cards
curl -H "X-Admin-Token: $T" http://host:8080/admin/cards
curl -X POST -H "X-Admin-Token: $T" -d '{"card":"VIP-0001"}' http://host:8080/admin/ban
curl -X POST -H "X-Admin-Token: $T" -d '{"card":"VIP-0001"}' http://host:8080/admin/unban
curl -X POST -H "X-Admin-Token: $T" -d '{"card":"VIP-0001","device":"abc"}' http://host:8080/admin/bind
curl -X POST -H "X-Admin-Token: $T" -d '{"card":"VIP-0001"}' http://host:8080/admin/unbind
```

## 客户端

```python
ticket = get_ticket(card, device_id)
driver.init_key(ticket)   # ioctl(OP_INIT_KEY, ticket)
```

## 强度

| 攻击 | 结果 |
| --- | --- |
| dump .ko | 只有公钥，伪造不了票据 |
| 改到期时间 | 签名失效 |
| 换设备字段 | 签名失效 |
| 抓包重放 | 票据有效期 ≤ 7 天，服务器可封卡 |
| patch 模块 | 只能改自己设备，无法伪造新票据 |

仍需注意：服务器必须走 HTTPS；票据可被分享到其他设备直到过期（内核不知道本机设备号），要更严可缩短有效期或在内核读取设备指纹。
