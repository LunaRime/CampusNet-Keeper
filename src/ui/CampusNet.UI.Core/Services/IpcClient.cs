using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

namespace CampusNet.UI.Services;

/// <summary>
/// 守护进程 IPC 客户端。
/// 
/// **契约（与 C++ 服务端一致，见 src/native/ipc/PipeServer.h）**：
/// <list type="bullet">
/// <item>一个连接只处理**一条**请求：读到换行 → 应答一条 → 关闭连接。因此本客户端
/// **每次请求新建一次连接**（连接本机管道开销极小）。</item>
/// <item>客户端必须在收到响应后**及时读取并关闭**：服务端断开时会丢弃未被读取的数据。</item>
/// <item>连接失败要重试两类瞬时错误：实例正忙（ERROR_PIPE_BUSY）与实例尚未建出
/// （ERROR_FILE_NOT_FOUND）。<see cref="NamedPipeClientStream.ConnectAsync(int, CancellationToken)"/>
/// 内部已覆盖这两种情况，这里再包一层总预算。</item>
/// </list>
/// 
/// 线程安全：可并发调用（无共享连接状态，请求 id 用原子自增）。
/// </summary>
public sealed class IpcClient : IAsyncDisposable
{
    private readonly int _connectTimeoutMs;
    private readonly int _readTimeoutMs;
    private int _nextId;

    public IpcClient(string? pipeName = null,
                     int connectTimeoutMs = PipeProtocol.DefaultConnectTimeoutMs,
                     int readTimeoutMs = PipeProtocol.DefaultReadTimeoutMs)
    {
        PipeName = string.IsNullOrEmpty(pipeName) ? PipeProtocol.PipeNameForCurrentUser() : pipeName;
        _connectTimeoutMs = connectTimeoutMs;
        _readTimeoutMs = readTimeoutMs;
    }

    /// <summary>本客户端使用的完整管道名。</summary>
    public string PipeName { get; }

    /// <summary>
    /// 守护进程是否在监听。任何失败都<b>返回 false</b>（不抛异常），供 UI 直接绑定状态。
    /// </summary>
    public async Task<bool> IsDaemonRunningAsync(CancellationToken ct = default)
    {
        try
        {
            var resp = await SendAsync("ping", null, ct).ConfigureAwait(false);
            return resp is not null;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>
    /// 发送一条命令并返回响应对象（<c>{"id":..,"ok":..,"data":..}</c> 或
    /// <c>{"id":..,"ok":false,"error":".."}</c>）。命令本身失败（ok=false）不算异常——
    /// 由调用方据 ok/error 处理。
    /// </summary>
    /// <exception cref="IpcException">连不上、写失败、读超时、响应非法或超过上限。</exception>
    public async Task<JsonNode?> SendAsync(string cmd, JsonObject? args = null,
                                           CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(cmd))
            throw new ArgumentException("命令不能为空", nameof(cmd));

        var id = Interlocked.Increment(ref _nextId);
        var request = new JsonObject { ["id"] = id, ["cmd"] = cmd };
        if (args is not null) request["args"] = args.DeepClone();  // DeepClone：避免把调用方的节点挂到本对象上
        var line = request.ToJsonString();  // ToJsonString 默认单行、不缩进

        using var pipe = await ConnectAsync(ct).ConfigureAwait(false);

        var payload = Encoding.UTF8.GetBytes(line + "\n");
        try
        {
            await pipe.WriteAsync(payload, ct).ConfigureAwait(false);
            await pipe.FlushAsync(ct).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException)
        {
            throw new IpcException($"发送请求失败（{PipeName}）：{ex.Message}", ex);
        }

        var responseLine = await ReadLineAsync(pipe, ct).ConfigureAwait(false);
        if (string.IsNullOrEmpty(responseLine))
            throw new IpcException($"未收到响应（{PipeName}）：守护进程可能已退出或未按协议应答");

        JsonNode? node;
        try
        {
            node = JsonNode.Parse(responseLine);
        }
        catch (Exception ex)
        {
            // 不把整条响应打进异常消息：里面可能含账号等敏感字段
            throw new IpcException($"响应不是合法 JSON（{PipeName}，{responseLine.Length} 字节）", ex);
        }
        if (node is not JsonObject)
            throw new IpcException($"响应不是 JSON 对象（{PipeName}）");
        return node;
    }

    /// <summary>
    /// 目前没有需要释放的常驻资源（每次请求用独立连接），保留该接口是为了与
    /// 计划中的调用形态一致，并为将来的连接池留出位置。
    /// </summary>
    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    private async Task<NamedPipeClientStream> ConnectAsync(CancellationToken ct)
    {
        var deadline = Environment.TickCount64 + _connectTimeoutMs;
        while (true)
        {
            var remaining = (int)Math.Max(50, deadline - Environment.TickCount64);
            var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut,
                                                PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(remaining, ct).ConfigureAwait(false);
                return pipe;
            }
            catch (Exception ex) when (ex is IOException or TimeoutException
                                          or UnauthorizedAccessException or OperationCanceledException
                                              && !ct.IsCancellationRequested)
            {
                pipe.Dispose();
                if (Environment.TickCount64 >= deadline)
                    throw new IpcException($"连接守护进程失败（{PipeName}），已重试 {_connectTimeoutMs}ms：{ex.Message}", ex);
                await Task.Delay(50, ct).ConfigureAwait(false);
            }
        }
    }

    /// <summary>
    /// 读到换行为止。按**字节**累积、最后整体按 UTF-8 解码——逐字节转 char 会把中文等
    /// 多字节字符切坏（args 里带中文时会立刻暴露）。
    /// </summary>
    private async Task<string> ReadLineAsync(Stream stream, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(_readTimeoutMs);

        var acc = new List<byte>(256);
        var buf = new byte[4096];
        try
        {
            while (true)
            {
                var n = await stream.ReadAsync(buf, timeout.Token).ConfigureAwait(false);
                if (n <= 0) break;  // 对端关闭
                for (var i = 0; i < n; i++)
                {
                    if (buf[i] == (byte)'\n')
                        return Encoding.UTF8.GetString(acc.ToArray());
                    acc.Add(buf[i]);
                }
                if (acc.Count > PipeProtocol.MaxResponseBytes)
                    throw new IpcException($"响应超过上限 {PipeProtocol.MaxResponseBytes} 字节（{PipeName}）");
            }
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            throw new IpcException($"读取响应超时（{PipeName}，{_readTimeoutMs}ms）");
        }
        catch (IOException ex)
        {
            throw new IpcException($"读取响应失败（{PipeName}）：{ex.Message}", ex);
        }
        return Encoding.UTF8.GetString(acc.ToArray());
    }
}
