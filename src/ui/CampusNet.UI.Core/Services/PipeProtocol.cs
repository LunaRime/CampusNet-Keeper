using System.Security.Principal;

namespace CampusNet.UI.Services;

/// <summary>
/// 管道名与线协议常量。**必须与 C++ 侧 <c>src/native/ipc/Protocol.h</c> 与
/// <c>PipeServer.h</c> 保持一致**，改动任一侧都要同步。
/// </summary>
public static class PipeProtocol
{
    /// <summary>管道基名（不含 <c>\\.\pipe\</c> 前缀与 SID 后缀）。</summary>
    public const string BaseName = "CampusNetKeeper";

    /// <summary>连接超时（毫秒）。含 BUSY / 尚未建实例时的重试预算。</summary>
    public const int DefaultConnectTimeoutMs = 1500;

    /// <summary>读一条响应的超时（毫秒）。</summary>
    public const int DefaultReadTimeoutMs = 3000;

    /// <summary>响应上限，与服务端一致（超过则服务端回「过大」错误）。</summary>
    public const int MaxResponseBytes = 32 * 1024;

    /// <summary>
    /// 完整管道名：<c>CampusNetKeeper.&lt;当前用户 SID&gt;</c>，与服务端
    /// <c>PipeNameForCurrentUser()</c> 同规则（服务端按用户 SID 隔离多用户机器）。
    /// 
    /// ⚠ <see cref="WindowsIdentity.GetCurrent()"/> 取的是**线程**令牌的用户：本程序不做
    /// 模拟（impersonation），故等同于进程令牌。若将来引入模拟，需改为取进程令牌的 SID
    /// （服务端用的是进程令牌），否则名字会对不上。
    /// </summary>
    public static string PipeNameForCurrentUser()
    {
        var sid = WindowsIdentity.GetCurrent().User?.Value;
        return string.IsNullOrEmpty(sid) ? BaseName : BaseName + "." + sid;
    }
}

/// <summary>IPC 层错误（连接失败、协议错误、响应超限等）。</summary>
public sealed class IpcException : Exception
{
    public IpcException(string message, Exception? inner = null) : base(message, inner) { }
}
