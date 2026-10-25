"""身份层专项自测：v1->v3 迁移、晚绑定拦截、openid 兜底拦截与升级、防绕过。

离线运行，不连网、不读真实 data.json（用临时文件，跑完自动清理）。
运行：py main.py --idtest
"""
import asyncio
import json
import os

from core.identity import Identity
from core.storage import Storage

TMP = "idtest.json"

FAIL: list = []


def check(name, cond, extra=""):
    print(("  OK   " if cond else "  FAIL ") + name + (f"  {extra}" if extra else ""))
    if not cond:
        FAIL.append(name)


def _v1_fixture() -> dict:
    """一份典型的旧版数据：同一人在两个群各有 openid，另有一个未绑定的人。"""
    return {
        # v1 结构 qq_bind：{openid: {qq, at}}
        "qq_bind": {
            "OID_A": {"qq": "111111111", "at": "2026-01-01 00:00"},
            "OID_A2": {"qq": "111111111", "at": "2026-01-02 00:00"},   # 同一人另一个群
            "OID_B": {"qq": "222222222", "at": "2026-01-03 00:00"},
        },
        "admins": {"G1": ["OID_A"]},
        "blacklist": {"G1": ["OID_A2"]},
        "super_admins": ["OID_A"],
        "kun_users": {"OID_A": {"username": "alice", "email": "a@x.com"}},
        "sign": {"G1": {"OID_A": {"total": 30, "streak": 3, "last": "2026-01-01", "count": 3},
                        "OID_A2": {"total": 10, "streak": 1, "last": "2026-01-01", "count": 1},
                        "OID_C": {"total": 5, "streak": 1, "last": "2026-01-01", "count": 1}}},
        "roster": {"G1": {"OID_A": {"username": "小明", "role": "admin"},
                          "OID_C": {"username": "未绑定的人", "role": "member"}}},
        "point_shop": {"cost": 100, "days": 30},
    }


def _load(data: dict):
    with open(TMP, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    st = Storage(TMP)
    return st, Identity(st)


def _migrate():
    print("\n[1] v1 -> v3 迁移")
    st, ident = _load(_v1_fixture())
    check("旧文件被识别为 v1", st.data["schema"] == 1, st.data["schema"])
    stats = ident.migrate_legacy()
    print("   迁移统计:", stats)
    check("schema 升到 3", st.data["schema"] == 3)
    check("qq_bind 改成「号 -> openid列表」",
          st.data["qq_bind"]["111111111"]["openids"] == ["OID_A", "OID_A2"],
          st.data["qq_bind"]["111111111"])
    check("同一人两个 openid 归一到同一 uid",
          ident.qq_of("OID_A") == ident.qq_of("OID_A2") == "111111111")
    check("群授权迁到 roles", st.data["roles"].get("G1") == {"111111111": "group_admin"},
          st.data.get("roles"))
    check("超管授权迁到 roles[*]", st.data["roles"].get("*") == {"111111111": "super"},
          st.data["roles"].get("*"))
    check("旧 admins/super_admins 键已清空",
          st.data["admins"] == {} and st.data["super_admins"] == [])
    check("blacklist 换成 QQ 号", st.data["blacklist"]["G1"] == ["111111111"], st.data["blacklist"])
    check("kun_users 换成 QQ 号", "111111111" in st.data["kun_users"])
    check("sign 同群合并取高分", st.data["sign"]["G1"]["111111111"]["total"] == 30)
    check("未绑定的人保持占位键", "openid:OID_C" in st.data["sign"]["G1"], list(st.data["sign"]["G1"]))
    check("roster 保留 openid 为键", "OID_A" in st.data["roster"]["G1"])
    check("roster 补写 qq", st.data["roster"]["G1"]["OID_A"].get("qq") == "111111111")
    check("point_shop 不受影响", st.data["point_shop"]["cost"] == 100)
    check("global_blacklist 初始化", st.data["global_blacklist"] == {})
    check("identity_ban / audit 初始化",
          st.data["identity_ban"] == {} and st.data["audit"] == [])

    before = json.dumps(st.data, ensure_ascii=False, sort_keys=True)
    _, ident2 = _load(json.loads(before))
    ident2.migrate_legacy()
    check("迁移幂等（重复加载结果一致）",
          json.dumps(ident2.storage.data, ensure_ascii=False, sort_keys=True) == before)
    return st, ident


def _migrate_schema_only():
    """回归 [1b]：v2 数据里没有任何可迁条目时，schema 仍要升级，返回值必须表达「需落盘」。

    曾经的 bug：migrate_legacy 返回 stats（迁移条目数），main.py 用 bool(stats) 判断
    是否 save。stats 为空 → 不落盘 → 磁盘 schema 永远停在旧版、每次重启重跑迁移。
    2026-10-12 线上实测踩到（线上 admins 的群列表为空 + super_admins 为空）。
    """
    print("\n[1b] 空授权表的 v2 -> v3（必须要求落盘）")
    d = {"schema": 2, "admins": {}, "super_admins": [],
         "blacklist": {"G1": ["123456789"]}, "qq_bind": {}, "roster": {}}
    st, ident = _load(d)
    check("识别为 v2", st.data["schema"] == 2, st.data["schema"])
    stats = ident.migrate_legacy()
    check("schema 升到 3", st.data["schema"] == 3)
    check("roles / identity_ban / audit 已建",
          st.data.get("roles") == {"*": {}} and st.data.get("identity_ban") == {}
          and st.data.get("audit") == [])
    check("旧 admins / super_admins 已清空",
          st.data["admins"] == {} and st.data["super_admins"] == [])
    check("返回值非空（= 需要落盘）", bool(stats), stats)
    check("已是 v3 时不再重复迁移（返回空）", ident.migrate_legacy() == {})


def _global_ban(st, ident):
    print("\n[2] 全局拉黑覆盖同一人全部身份，且晚绑定也拦")
    ident.global_block("111111111", "999999999", "刷广告")
    check("已登记身份立刻命中", ident.is_globally_blocked("OID_A"))
    check("同一人另一群身份也命中", ident.is_globally_blocked("OID_A2"))
    code, _ = ident.bind("OID_NEW", "111111111")
    check("被拉黑的号不能再被绑定", code == "qq_taken", code)

    print("\n[3] 先拉黑未登记的 QQ 号，对方之后绑定即被拦")
    ident.global_block("333333333", "999999999", "恶意刷屏")
    check("未登记时无人命中", ident.is_globally_blocked("OID_D") is False)
    code, _ = ident.bind("OID_D", "333333333")
    check("绑定成功", code == "ok", code)
    check("绑定后立即被拦", ident.is_globally_blocked("OID_D"))

    print("\n[4] 私聊（C2C）同样拦截")

    def blocked(openid, group=""):
        uid = ident.uid_of_openid(openid)
        return (ident.is_globally_blocked(openid)
                or uid in st.data["blacklist"].get(group or "", []))

    check("私聊被拉黑的号拦截", blocked("OID_A"))
    check("私聊未被拉黑的号放行", not blocked("OID_B"))
    check("换群后的同一人也拦", blocked("OID_A", "G_OTHER"))


def _identity_ban(st, ident):
    print("\n[4b] openid 兜底拦截：没绑 QQ 号也能全局封，绑定后自动升级")
    ok, msg = ident.identity_block("OID_X", "999999999", "捣乱")
    check("openid 拦截生效", ok and ident.is_globally_blocked("OID_X"))
    check("重复拦截有提示", ident.identity_block("OID_X", "999999999")[0] is False)
    code, _ = ident.bind("OID_X", "777777777")
    check("被兜底拦截的身份仍可绑定（防误伤可解）", code == "ok", code)
    check("绑定后升级为按号封", "777777777" in st.data["global_blacklist"],
          st.data["global_blacklist"])
    check("升级后 openid 记录已清除", "OID_X" not in st.data["identity_ban"])
    check("升级后原身份仍被拦", ident.is_globally_blocked("OID_X"))

    print("\n[4c] 超管登记 QQ（link_openid）同样触发升级")
    ident.identity_block("OID_Y", "999999999", "广告")
    ok, _ = ident.link_openid("OID_Y", "888888888", create=True)
    check("登记成功", ok)
    check("升级为按号封", "888888888" in st.data["global_blacklist"])
    check("openid 记录已清除", "OID_Y" not in st.data["identity_ban"])
    ok, msg = ident.identity_unblock("OID_X")
    check("已升级的 openid 无法再单独解（记录已不存在）", ok is False, msg)
    ok, _ = ident.identity_unblock("OID_GONE")
    check("解不存在的拦截有提示", ok is False)
    ident.identity_block("OID_Z", "999999999", "测试")
    ok, _ = ident.identity_unblock("OID_Z")
    check("未升级的 openid 拦截可解除", ok and not ident.is_globally_blocked("OID_Z"))


def _claim(st):
    print("\n[5] 未绑定时攒的数据在绑定后被认领")
    st.data["sign"]["G1"]["openid:OID_E"] = {"total": 42, "streak": 7, "last": "", "count": 9}
    st.data["kun_users"]["openid:OID_E"] = {"username": "eve"}
    st.claim_user("openid:OID_E", "444444444")
    check("积分认领", st.data["sign"]["G1"].get("444444444", {}).get("total") == 42)
    check("占位键已清掉", "openid:OID_E" not in st.data["sign"]["G1"])
    check("账号认领", st.data["kun_users"]["444444444"]["username"] == "eve")


def _no_evade(ident):
    print("\n[6] 解绑 / 改绑防绕过")
    ok, msg = ident.unbind_qq("111111111")
    check("拉黑中的号不允许解绑", ok is False, msg)
    ident.global_unblock("111111111")
    ok, _ = ident.unbind_qq("111111111")
    check("解黑后可解绑", ok is True)
    check("解绑后 uid 变回占位键", ident.uid_of_openid("OID_A") == "openid:OID_A")
    code, _ = ident.bind("OID_A", "111111111")
    check("可重新绑回原号", code == "ok", code)

    print("\n[7] 输入校验")
    bad = [w for w in ("", "abc", "1234", "1234567890123", "12 34")
           if ident.bind("OID_F", w)[0] != "format"]
    check("拒绝所有非法 QQ 号", not bad, bad)


def _rbac():
    print("\n[8] RBAC 权限层：角色分层 / 平台身份开关 / 目标保护 / 权限节点")
    from core.permissions import Permissions

    data = {
        "schema": 3,
        "qq_bind": {"111111111": {"qq": "111111111", "at": "", "openids": ["OID_OWN"]}},
        "roles": {"*": {"222222222": "super", "777777777": "super"},
                  "G1": {"333333333": "group_admin", "888888888": "group_admin",
                         "666666666": "member"}},
        "blacklist": {}, "global_blacklist": {}, "identity_ban": {}, "audit": [],
    }
    with open(TMP, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    st = Storage(TMP)
    ident = Identity(st)

    class FakeBot:
        def __init__(self, cfg):
            self.config = cfg
            self.identity = ident
            self.storage = st

    class Ctx:
        def __init__(self, uid, sender, group="", member_role=""):
            self.uid, self.sender = uid, sender
            self.group_openid, self.member_role = group, member_role
            self.role_label, self.level = "普通用户", 0

    cfg = {"owner_qq": "111111111", "admins": ["444444444"], "permissions": {}}
    bot = FakeBot(cfg)
    perms = Permissions(bot)

    check("owner 解析（config owner_qq）", perms.resolve_ids("111111111", "OID_OWN")[1] == 100)
    check("owner_qq 字符串写法归一成列表", perms.owners() == ["111111111"], perms.owners())
    check("super 解析（config admins）", perms.resolve_ids("444444444", "OID_A")[1] == 80)
    check("super 解析（roles[*]）", perms.resolve_ids("222222222", "OID_B")[1] == 80)
    check("群管解析（roles[群]）", perms.resolve_ids("333333333", "OID_C", "G1")[1] == 40)
    check("群管不出本群", perms.resolve_ids("333333333", "OID_C", "G2")[1] == 0)
    check("普通人", perms.resolve_ids("555555555", "OID_D", "G1")[1] == 0)

    ctx_plat = Ctx("openid:OID_E", "OID_E", "G1", "owner")
    check("平台群主身份默认不提权", perms.resolve(ctx_plat)[1] == 0)
    cfg["permissions"] = {"trust_platform_roles": True}
    check("trust 开启后平台群主=群管", perms.resolve(ctx_plat)[1] == 40)
    check("显式 member 压过平台群主身份",
          perms.resolve(Ctx("666666666", "OID_F", "G1", "owner"))[1] == 0)
    cfg["permissions"] = {}

    # 目标保护（防权限倒挂）
    ctx_super = Ctx("222222222", "OID_B", "G1")
    ctx_super.role_label, ctx_super.level = perms.resolve(ctx_super)
    check("不能对主人操作", perms.target_guard(ctx_super, "111111111") is not None)
    check("不能对同级超管操作", perms.target_guard(ctx_super, "777777777") is not None)
    check("操作者本人放行（自卫）", perms.target_guard(ctx_super, "222222222") is None)
    check("super 可管群管", perms.target_guard(ctx_super, "333333333") is None)
    ctx_group = Ctx("333333333", "OID_C", "G1")
    ctx_group.role_label, ctx_group.level = perms.resolve(ctx_group)
    check("群管不能封超管", perms.target_guard(ctx_group, "222222222") is not None)
    check("群管不能封另一个群管（同级）", perms.target_guard(ctx_group, "888888888") is not None)
    check("群管可封用户", perms.target_guard(ctx_group, "555555555") is None)
    check("未绑定占位键目标放行", perms.target_guard(ctx_group, "openid:OID_D") is None)

    # 权限节点
    check("群管有 group.ban", perms.allows(ctx_group, "group.ban"))
    check("群管无 global.ban", not perms.allows(ctx_group, "global.ban"))
    check("超管有 global.ban", perms.allows(ctx_super, "global.ban"))
    check("超管无 role.super（仅主人）", not perms.allows(ctx_super, "role.super"))
    ctx_owner = Ctx("111111111", "OID_OWN")
    ctx_owner.role_label, ctx_owner.level = perms.resolve(ctx_owner)
    check("主人有 role.super", perms.allows(ctx_owner, "role.super"))

    # 多主人（owner_qq 数组写法）：两个主人平级、互保、都持 role.super
    cfg["owner_qq"] = ["111111111", "999999999"]
    perms2 = Permissions(bot)
    check("多主人：数组归一", perms2.owners() == ["111111111", "999999999"], perms2.owners())
    check("多主人：第二个主人同为主人",
          perms2.resolve_ids("999999999", "OID_G2")[1] == 100)
    check("多主人：is_owner_uid 命中两个号",
          perms2.is_owner_uid("111111111") and perms2.is_owner_uid("999999999"))
    check("多主人：不相干的号不是主人", not perms2.is_owner_uid("555555555"))
    ctx_owner2 = Ctx("999999999", "OID_G2", "G1")
    ctx_owner2.role_label, ctx_owner2.level = perms2.resolve(ctx_owner2)
    check("多主人：主人2 也有 role.super", perms2.allows(ctx_owner2, "role.super"))
    check("多主人：主人1 不能封主人2", perms2.target_guard(ctx_owner, "999999999") is not None)
    check("多主人：主人2 不能封主人1", perms2.target_guard(ctx_owner2, "111111111") is not None)
    ok_revoke, _msg = asyncio.run(perms2.revoke_super(ctx_owner, "999999999"))
    check("多主人：config 来源的主人不能撤销超管身份", ok_revoke is False)
    check("空数组不产生主人", Permissions(FakeBot(
        {"owner_qq": [], "admins": [], "permissions": {}})).owners() == [])
    cfg["owner_qq"] = "111111111"

    # 展示层：角色名必须中文；同一个人的 QQ 号与 openid 必须归到同一个人
    check("resolve 给中文角色名（不再漏 owner/super 英文）",
          ctx_group.role_label == "群管理员", ctx_group.role_label)
    check("归并：QQ 号 key 是自身", perms.person_key("111111111") == "111111111")
    check("归并：已绑定 openid 归到同一个 QQ 号",
          perms.person_key("OID_OWN") == "111111111", perms.person_key("OID_OWN"))
    check("归并：未绑定的裸 openid 保留自身",
          perms.person_key("DEADBEEFDEADBEEFDEADBEEFDEADBEEF")
          == "DEADBEEFDEADBEEFDEADBEEFDEADBEEF")
    got = perms.persons(["111111111", "OID_OWN", "444444444"])
    check("归并：同一人只留一条（111111111 与 OID_OWN 是同一人）", len(got) == 2, got)
    check("归并：空值被跳过", perms.persons(["", "", "OID_OWN"]) == perms.persons(["OID_OWN"]))
    print("✅ RBAC 单测通过")


def _confirm():
    """二次确认槽 + changelog 归一（纯函数，不需要 Bot）。"""
    from core.confirm import PendingConfirms
    from core.release_watch import clean_changelog

    c = PendingConfirms()

    async def _noop(ctx):
        return None

    c.arm("G1:1", command="x", summary="s", run=_noop, uid="1")
    check("确认槽：取走即删除（一次性）", c.take("G1:1") is not None and c.peek("G1:1") is None)
    check("确认槽：空槽返回 None", c.peek("G1:1") is None)
    c.arm("G1:1", command="a", summary="旧", run=_noop, uid="1")
    c.arm("G1:1", command="b", summary="新", run=_noop, uid="1")
    check("确认槽：同会话再次下发覆盖旧槽", c.peek("G1:1")["summary"] == "新")
    check("确认槽：会话隔离（别人的 key 拿不到）", c.peek("G1:2") is None)
    c.arm("G1:1", command="a", summary="s", run=_noop, uid="1", perm="server.manage")
    check("确认槽：记录权限节点（确认时二次校验）", c.peek("G1:1")["perm"] == "server.manage")

    # changelog 归一：与服务端邮件渲染同一套规则
    got = clean_changelog("1. 修复登录双发\n· 主页快捷操作\n- 体积优化\n\n本次更新说明")
    check("changelog：剥编号与项目符号、丢说明行",
          got == ["修复登录双发", "主页快捷操作", "体积优化"], got)
    check("changelog：空输入返回空列表", clean_changelog("") == [])
    print("✅ 二次确认 / changelog 归一 单测通过")


def run():
    try:
        st, ident = _migrate()
        _migrate_schema_only()
        _global_ban(st, ident)
        _identity_ban(st, ident)
        _claim(st)
        _no_evade(ident)
        _rbac()
        _confirm()
    finally:
        for p in (TMP, TMP + ".bak"):
            if os.path.exists(p):
                os.remove(p)
    print("\n" + "=" * 50)
    if FAIL:
        print(f"身份层自测失败 {len(FAIL)} 项：{', '.join(FAIL)}")
        raise SystemExit(1)
    print("身份层自测全部通过 ✅（v1->v3 迁移 / 全局拉黑 / openid 兜底拦截与升级 / 晚绑定拦截 / RBAC / 防绕过）")