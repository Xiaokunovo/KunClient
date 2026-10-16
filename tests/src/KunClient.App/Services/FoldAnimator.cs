using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Services;

/// <summary>
/// 给所有 <c>ToolFold</c> 折叠区加**展开 / 收起补间**。
/// <para>
/// 现状：<see cref="Expander"/> 是硬切 —— 展开瞬间内容出现、收起瞬间消失，页面上很"跳"。
/// 而项目自己的动效红线写着「『展开/收起』类变化一律补间，不再硬切」，
/// 动效工具箱（<see cref="Motion"/> 的 Reveal/Conceal）也都齐了，只是**没有任何页面在用**。
/// </para>
/// <para>
/// 🔴 为什么只淡入淡出 + 8px 位移、**不补间高度**：高度补间要改 Expander 模板 +
/// 用 <c>GridLengthAnimation</c> 驱动行高，是另一个改动面（且行高有硬下限、溢出逻辑会互相干扰）。
/// 这里先把最扎眼的"硬切"消掉：收起时先淡出再折叠，读起来就是顺的。
/// </para>
/// <para>
/// 🔴 走 <see cref="BasePage"/> 统一挂钩，而不是在 <c>ToolFold</c> 样式里挂 <c>EventSetter</c>：
/// 样式是全项目共享的，<c>EventSetter</c> 的 Handler 必须写在**每个页面**的 code-behind 里，
/// 做不到；而所有页面都继承 <see cref="BasePage"/>，在那里挂一次就全覆盖。
/// </para>
/// </summary>
public static class FoldAnimator
{
    /// <summary>已挂钩的 Expander（用 ConditionalWeakTable 避免静态集合持有死对象）。</summary>
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<Expander, object> _hooked = new();

    /// <summary>
    /// 已经走过的根（页面）。**每个根只走一次**：折叠区全部来自 XAML（代码里没有任何
    /// <c>new Expander</c>），首次 Loaded 就能一次找全；而切页签时每次都全树下钻是纯开销 ——
    /// 页面几百上千个节点、逻辑树 + 视觉树双下钻、每个节点 yield 一个枚举器。
    /// 运行期才展开出来的深层折叠区由 <see cref="OnIsExpandedChanged"/> 里那次
    /// <c>Attach(exp)</c> 兜住（它以 exp 为根，第一次展开时才会走到）。
    /// </summary>
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<DependencyObject, object> _walked = new();

    private const int RevealMs = 200;
    private const int ConcealMs = 150;
    private const double Slide = 8;   // ≤16px（动效红线）

    /// <summary>把 <paramref name="root"/> 子树里所有折叠区挂上补间；**已经走过的根直接跳过**。</summary>
    public static void Attach(DependencyObject root)
    {
        if (root == null) return;
        if (_walked.TryGetValue(root, out _)) return;
        _walked.Add(root, new object());

        foreach (var expander in FindAll(root))
        {
            if (_hooked.TryGetValue(expander, out _)) continue;
            _hooked.Add(expander, new object());
            // 🔴 WPF 的 Expander **没有**公开的 IsExpandedChanged 事件（只有 IsExpanded 依赖属性），
            //    所以用 DependencyPropertyDescriptor 挂变更通知 —— 这是 DP 属性的标准做法。
            System.ComponentModel.DependencyPropertyDescriptor
                .FromProperty(Expander.IsExpandedProperty, typeof(Expander))
                ?.AddValueChanged(expander, OnIsExpandedChanged);
        }
    }

    private static void OnIsExpandedChanged(object? sender, EventArgs e)
    {
        if (sender is not Expander exp) return;
        // 折叠区内容在模板的 Body 里；拿不到就退化成只动 expander 自身
        var body = FindBody(exp) as FrameworkElement;

        if (exp.IsExpanded)
        {
            if (body != null)
            {
                body.Visibility = Visibility.Visible;
                body.Opacity = 0;                    // 基值：RevealAsync 内部也写，但先落基值更稳
                _ = Motion.RevealAsync(body, RevealMs, -Slide);
            }
            // 展开后子树里可能还有折叠区（原先它们不在可视树里，扫不到）
            Attach(exp);
        }
        else if (body != null)
        {
            // 淡出完再折叠（ConcealAsync 的 collapseAfter 就是干这个的）
            _ = Motion.ConcealAsync(body, ConcealMs, -Slide);
        }
        else
        {
            exp.Visibility = Visibility.Collapsed;
        }
    }

    /// <summary>
    /// 取 Expander 模板里的内容容器。
    /// 🔴 本项目 <c>ToolFold</c> 模板里的内容容器叫 <c>Body</c>（不是 WPF 默认的
    /// <c>SiteOfContent</c> / <c>ContentSite</c>），所以三个名字都要试，最后再兜底扫模板里的
    /// 第一个 ContentPresenter —— 只试默认名会永远取不到 body，动画就静默不发生。
    /// </summary>
    private static object? FindBody(Expander exp)
    {
        if (exp.Template == null) exp.ApplyTemplate();
        foreach (var name in new[] { "Body", "SiteOfContent", "ContentSite" })
            if (exp.Template?.FindName(name, exp) is { } byName) return byName;
        // 兜底：模板里第一个 ContentPresenter
        return Descendants(exp).OfType<ContentPresenter>().FirstOrDefault();
    }

    /// <summary>
    /// 逻辑树 + 视觉树并集下钻，**去重 + 迭代**：同一个节点既可能是逻辑子节点又可能是可视子节点，
    /// 原来的写法会把它走两遍（递归 yield，每个节点一次枚举器分配）。
    /// 有 visited 兜底后也不再需要深度上限（逻辑树不会成环）。
    /// </summary>
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        var seen = new HashSet<DependencyObject>(ReferenceEqualityComparer.Instance);
        var stack = new Stack<DependencyObject>();
        stack.Push(root);
        while (stack.Count > 0)
        {
            var cur = stack.Pop();
            if (cur == null || !seen.Add(cur)) continue;
            yield return cur;

            if (cur is System.Windows.Media.Visual || cur is System.Windows.Media.Media3D.Visual3D)
            {
                var n = VisualTreeHelper.GetChildrenCount(cur);
                for (var i = 0; i < n; i++) stack.Push(VisualTreeHelper.GetChild(cur, i));
            }
            foreach (var child in LogicalTreeHelper.GetChildren(cur))
                if (child is DependencyObject d) stack.Push(d);
        }
    }

    private static IEnumerable<Expander> FindAll(DependencyObject root)
    {
        foreach (var d in Descendants(root))
            if (d is Expander e) yield return e;
    }
}
