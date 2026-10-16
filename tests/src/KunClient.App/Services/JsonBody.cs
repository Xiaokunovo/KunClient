using System.Text.Json.Nodes;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 构造 JSON 请求体。
    /// <para>
    /// <b>为什么不用 `JsonSerializer.Serialize(new { a, b })`：</b>
    /// 匿名类型**只有带参数的构造函数**（编译器生成的），而 <c>System.Text.Json</c> 在构建
    /// <c>JsonTypeInfo</c> 时会去读构造函数的**参数名**。混淆器（Obfuscar 的 <c>RenameParams</c>）
    /// 会把「没有自定义特性的参数」的名字**清成 null**，于是 STJ 直接抛：
    /// <code>
    /// NotSupportedException: The deserialization constructor for type '...'
    /// contains parameters with null names. This might happen because the parameter
    /// names have been trimmed by ILLink. Consider using the source generated serializer instead.
    /// </code>
    /// 关键在于它**编译期毫无征兆、只在运行期炸**，而且炸的是「登录 / 注册 / 额度闸门」这些
    /// 核心通路（2026-09-25 实测：混淆版上线后闸门返回
    /// <c>Reason=offline</c>，因为异常被 <c>RequestGate</c> 的 <c>catch { }</c> 吞了，表面看像网络不通）。
    /// </para>
    /// <para>
    /// <c>JsonObject</c> 走的是 STJ 的内置转换器，不做构造函数反射，因此完全不受参数名影响。
    /// 值也是**逐项显式**写入的，不会有「属性名被改名」之类的隐患。
    /// </para>
    /// <para>
    /// <b>同一类坑还要注意：</b>任何**只有带参构造函数**的类型（<c>record</c>、只写主构造函数的
    /// class）都不该交给 STJ 的反射序列化器 —— 要么像这里一样绕开，要么改用 source-generated 序列化。
    /// 另外，`<c>JsonSerializer.Deserialize&lt;JsonElement&gt;</c>` 是安全的（JsonElement 本身没有构造函数参数）。
    /// </para>
    /// </summary>
    internal static class JsonBody
    {
        /// <summary>按传入顺序拼出 JSON 对象串；值为 null 时输出 <c>null</c>。</summary>
        public static string Of(params (string Key, object? Value)[] pairs)
        {
            var obj = new JsonObject();
            foreach (var (key, value) in pairs)
            {
                obj[key] = value switch
                {
                    null => (JsonNode?)null,
                    string s => JsonValue.Create(s),
                    bool b => JsonValue.Create(b),
                    int i => JsonValue.Create(i),
                    long l => JsonValue.Create(l),
                    // 兜底：本项目只用到上面几种类型；真出现别的类型时按字符串写，
                    // 服务端校验会报错、比静默漏字段好定位。
                    _ => JsonValue.Create(value.ToString()),
                };
            }
            return obj.ToJsonString();
        }
    }
}
