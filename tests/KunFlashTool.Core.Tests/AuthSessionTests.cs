using System;
using System.IO;
using System.Linq;
using KunFlashTool.App.Services;
using KunFlashTool.Core.Configuration;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 用例①：未勾选「记住密码」时闸门仍取得到 token（组 A）。
///
/// 病因：以前 token 只在勾选 RememberMe 时才随 SaveCredentials 落盘，
/// 不勾选 → 磁盘上没有登录态 → MembershipService.RequestGate 取不到 token →
/// noauth → EDL 的 GuardAndConsume 直接 return，**引导程序压根没发**。
/// 修复后 token 无论勾选与否都进 AuthSession 内存（A4），密码才受勾选控制（A2）。
/// </summary>
public class AuthSessionTests
{
    [Fact]
    public void ResolveToken_ReturnsMemoryToken_EvenWhenPasswordNotPersisted()
    {
        // 模拟 LoginDialog.CompleteLogin 在「不勾选记住密码」时的行为（A4）：
        // AuthSession.Set 一定执行，SaveCredentials 的 persistPassword 才看勾选。
        try
        {
            AuthSession.Set("__gate_test_user__", "token-in-memory", DateTime.UtcNow.AddHours(1));

            // 闸门（MembershipService.RequestGate）走的就是这条取 token 的路径
            Assert.Equal("token-in-memory", AuthSession.ResolveToken());
            Assert.True(AuthSession.IsAuthenticated);
            Assert.Equal("__gate_test_user__", AuthSession.Username);
        }
        finally
        {
            AuthSession.Clear();
        }
    }

    [Fact]
    public void Clear_RemovesMemoryToken_SoGateFallsBackToDisk()
    {
        try
        {
            AuthSession.Set("__gate_test_user__", "token-in-memory", DateTime.UtcNow.AddHours(1));
            Assert.Equal("__gate_test_user__", AuthSession.Username);

            AuthSession.Clear();
            // 内存里的用户名/凭证必须清干净
            Assert.Equal("", AuthSession.Username);
            // 内存 token 已清 → ResolveToken 只可能拿到磁盘兜底值（本机可能真有 login.dat），
            // 绝不能再返回刚才那个内存 token。两条路径都要能被区分，才谈得上"内存优先、磁盘兜底"。
            Assert.NotEqual("token-in-memory", AuthSession.ResolveToken());
        }
        finally
        {
            AuthSession.Clear();
        }
    }

    [Fact]
    public void SaveCredentials_NeverPersistsPassword_PersistLoginControlsToken()
    {
        // 2026-10-09（源码审计后收紧）：密码**永不落盘**（原来只是"不勾选时丢"），
        // 勾选框也随之改名 —— persistLogin 控制的是「要不要把 token 写进磁盘」。
        // 三级路径与 LoginStorage/ClearCredentials 同源；测试要写真实状态目录，
        // 先整份备份、finally 原样还原 —— 绝不能污染开发者机器上的登录态。
        var paths = new[]
        {
            AppStatePaths.Combine("login.dat"),
            AppStatePaths.Legacy("login.dat"),
            AppStatePaths.Legacy2("login.dat"),
        };
        var backup = paths.Select(p => File.Exists(p) ? File.ReadAllBytes(p) : null).ToArray();

        try
        {
            // ① 勾选「记住登录状态」：token 落盘，但**密码依然不落盘**
            LoginStorage.SaveCredentials("__persist_test_user__", "tok-disk",
                DateTime.UtcNow.AddHours(1), persistLogin: true);

            var loaded = LoginStorage.LoadCredentials();
            Assert.NotNull(loaded);
            Assert.Equal("tok-disk", loaded!.Token);
            // 核心断言 1：密码从不落盘
            Assert.Equal("", loaded.Password);

            // 内存（A4）+ 磁盘兜底两条路都能把 token 交给闸门
            AuthSession.Set("__persist_test_user__", "tok-disk", DateTime.UtcNow.AddHours(1));
            Assert.Equal("tok-disk", AuthSession.ResolveToken());
            AuthSession.Clear();
            Assert.Equal("tok-disk", AuthSession.ResolveToken());

            // ② 不勾选：连登录态都不落盘 —— 磁盘上读不到任何东西
            LoginStorage.SaveCredentials("__persist_test_user__", "tok-disk",
                DateTime.UtcNow.AddHours(1), persistLogin: false);
            Assert.Null(LoginStorage.LoadCredentials());   // 核心断言 2
        }
        finally
        {
            AuthSession.Clear();
            for (var i = 0; i < paths.Length; i++)
            {
                try
                {
                    if (backup[i] == null)
                    {
                        if (File.Exists(paths[i])) File.Delete(paths[i]);
                    }
                    else
                    {
                        File.WriteAllBytes(paths[i], backup[i]!);
                    }
                }
                catch { }
            }
        }
    }
}