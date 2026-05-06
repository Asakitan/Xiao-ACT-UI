using Google.Protobuf;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Decoder for a single notify method id. Implementations parse the inner
/// payload (post-c3SB header strip, post-zstd decompression) and emit one
/// or more <see cref="ParserEvent"/> through the supplied callback.
///
/// Mirrors the per-method <c>_on_*</c> handlers in
/// <c>sao_auto/packet_parser.py</c>. A registry of these is the C# port of
/// the dispatch table — see <see cref="MethodDecoderRegistry"/>.
/// </summary>
public interface IMethodDecoder
{
    /// <summary>The notify method id this decoder handles.</summary>
    int MethodId { get; }

    /// <summary>
    /// Decode the inner payload and emit any resulting events.
    /// Implementations MUST be exception-safe; the registry catches throws
    /// and logs them, but a clean decoder simply returns on malformed input.
    /// </summary>
    void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit);
}

/// <summary>
/// Convenience base for proto-backed decoders. Subclasses just supply the
/// <see cref="MessageParser{T}"/> and the conversion from message → event.
/// </summary>
public abstract class ProtoMethodDecoder<TMessage> : IMethodDecoder
    where TMessage : IMessage<TMessage>
{
    public abstract int MethodId { get; }
    protected abstract MessageParser<TMessage> Parser { get; }

    public void Decode(ReadOnlySpan<byte> body, double timestampSeconds, Action<ParserEvent> emit)
    {
        TMessage msg;
        try
        {
            // Google.Protobuf does not yet take ReadOnlySpan; copy is unavoidable
            // until v4. The arrays here are short-lived and per-frame.
            msg = Parser.ParseFrom(body.ToArray());
        }
        catch (InvalidProtocolBufferException)
        {
            return;
        }
        Project(msg, timestampSeconds, emit);
    }

    protected abstract void Project(TMessage message, double timestampSeconds, Action<ParserEvent> emit);
}
