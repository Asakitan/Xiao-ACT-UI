using System.Collections.Immutable;

namespace SaoAuto.Core.Automation;

/// <summary>
/// P3 / CMD-001..008 — Skeleton port of the Python Commander surface.
/// Tracks the local team roster (leader + members) plus per-member identity
/// snapshots (uid / name / profession / fight point / level / online flag),
/// builds an immutable <see cref="CommanderSnapshot"/> on every update, and
/// fans the snapshot through <see cref="SnapshotChanged"/>. Caller wires
/// the event to <c>BridgeEvents.CommanderUpdated</c>.
///
/// <para>Scope: covers the data feed Python's <c>CommanderPanel</c> at
/// sao_gui_commander.py:67-117 consumes — same dict shape with the
/// {leader_id, members[]} keys. CharTeam decoder integration is the
/// PacketParser-side counterpart (extends <c>SyncContainerDataDecoder</c>
/// when CharBase.TeamInfo proto fields are decoded). Cloud-sync /
/// invitation flow / mute / leader-change requests stay in
/// <see cref="Bridge.PacketBridge"/>-driven extensions that arrive after
/// the proto coverage closes.</para>
/// </summary>
public sealed class CommanderEngine
{
    private readonly object _gate = new();
    private CommanderSnapshot _current = CommanderSnapshot.Empty;

    /// <summary>Fired on every snapshot mutation. Caller wires this to
    /// the bridge event surface so JS panels see commander updates.</summary>
    public event Action<CommanderSnapshot>? SnapshotChanged;

    public CommanderSnapshot Current { get { lock (_gate) return _current; } }

    /// <summary>
    /// CMD-001/002: install / refresh the team roster. Mirrors Python's
    /// CharTeam decoder consumer at packet_parser.py:4640-4649. Idempotent
    /// — repeated calls with an unchanged roster produce no event.
    /// </summary>
    public void UpdateTeam(
        long teamId,
        long leaderUid,
        string leaderName,
        IReadOnlyList<CommanderMember> members,
        long selfUid = 0)
    {
        CommanderSnapshot? toEmit = null;
        lock (_gate)
        {
            var nextMembers = members?.ToImmutableArray() ?? ImmutableArray<CommanderMember>.Empty;
            var next = new CommanderSnapshot
            {
                TeamId = teamId,
                LeaderUid = leaderUid,
                LeaderName = leaderName ?? string.Empty,
                Members = nextMembers,
                SelfUid = selfUid,
                LastUpdatedAt = DateTimeOffset.UtcNow,
            };
            if (SameContent(_current, next)) return;
            _current = next;
            toEmit = next;
        }
        if (toEmit is not null)
        {
            try { SnapshotChanged?.Invoke(toEmit); } catch { /* swallow */ }
        }
    }

    /// <summary>CMD-005: patch a single member's identity fields without
    /// rebuilding the whole roster. Used when SyncContainerData / PlayerAttr
    /// trickle in per-uid updates between full CharTeam pushes.</summary>
    public void PatchMember(long uid, Action<CommanderMember.Builder> patch)
    {
        ArgumentNullException.ThrowIfNull(patch);
        CommanderSnapshot? toEmit = null;
        lock (_gate)
        {
            for (var i = 0; i < _current.Members.Length; i++)
            {
                if (_current.Members[i].Uid != uid) continue;
                var b = CommanderMember.Builder.From(_current.Members[i]);
                patch(b);
                var patched = b.Build();
                if (patched == _current.Members[i]) return;
                var nextMembers = _current.Members.SetItem(i, patched);
                _current = _current with
                {
                    Members = nextMembers,
                    LastUpdatedAt = DateTimeOffset.UtcNow,
                };
                toEmit = _current;
                break;
            }
        }
        if (toEmit is not null)
        {
            try { SnapshotChanged?.Invoke(toEmit); } catch { /* swallow */ }
        }
    }

    /// <summary>CMD-008: clear the roster (party disbanded). Mirrors
    /// Python's CharTeam disband path at packet_parser.py:4439.</summary>
    public void Clear()
    {
        CommanderSnapshot? toEmit = null;
        lock (_gate)
        {
            if (_current.Members.IsEmpty && _current.TeamId == 0) return;
            _current = CommanderSnapshot.Empty;
            toEmit = _current;
        }
        if (toEmit is not null)
        {
            try { SnapshotChanged?.Invoke(toEmit); } catch { /* swallow */ }
        }
    }

    private static bool SameContent(CommanderSnapshot a, CommanderSnapshot b)
    {
        if (a.TeamId != b.TeamId) return false;
        if (a.LeaderUid != b.LeaderUid) return false;
        if (a.LeaderName != b.LeaderName) return false;
        if (a.SelfUid != b.SelfUid) return false;
        if (a.Members.Length != b.Members.Length) return false;
        for (var i = 0; i < a.Members.Length; i++)
        {
            if (a.Members[i] != b.Members[i]) return false;
        }
        return true;
    }
}

/// <summary>
/// P3 / CMD — immutable snapshot of the local team roster. Single object
/// fanned through <see cref="CommanderEngine.SnapshotChanged"/>.
/// </summary>
public sealed record CommanderSnapshot
{
    public long TeamId { get; init; }
    public long LeaderUid { get; init; }
    public string LeaderName { get; init; } = string.Empty;
    public ImmutableArray<CommanderMember> Members { get; init; } = ImmutableArray<CommanderMember>.Empty;
    public long SelfUid { get; init; }
    public DateTimeOffset LastUpdatedAt { get; init; }

    public static readonly CommanderSnapshot Empty = new();
}

/// <summary>P3 / CMD — per-member identity row.</summary>
public sealed record CommanderMember(
    long Uid,
    string Name)
{
    public int ProfessionId { get; init; }
    public string ProfessionName { get; init; } = string.Empty;
    public int Level { get; init; }
    public int FightPoint { get; init; }
    public bool Online { get; init; } = true;
    public bool IsLeader { get; init; }
    public bool IsSelf { get; init; }

    /// <summary>Mutable builder used by <see cref="CommanderEngine.PatchMember"/>
    /// so callers can splice one or two fields without rebuilding the
    /// whole record.</summary>
    public sealed class Builder
    {
        private long _uid;
        private string _name = string.Empty;
        private int _professionId;
        private string _professionName = string.Empty;
        private int _level;
        private int _fightPoint;
        private bool _online = true;
        private bool _isLeader;
        private bool _isSelf;

        public static Builder From(CommanderMember src) => new()
        {
            _uid = src.Uid,
            _name = src.Name,
            _professionId = src.ProfessionId,
            _professionName = src.ProfessionName,
            _level = src.Level,
            _fightPoint = src.FightPoint,
            _online = src.Online,
            _isLeader = src.IsLeader,
            _isSelf = src.IsSelf,
        };

        public Builder WithName(string n) { _name = n ?? string.Empty; return this; }
        public Builder WithProfession(int id, string name)
        {
            _professionId = id;
            _professionName = name ?? string.Empty;
            return this;
        }
        public Builder WithLevel(int v) { _level = v; return this; }
        public Builder WithFightPoint(int v) { _fightPoint = v; return this; }
        public Builder WithOnline(bool v) { _online = v; return this; }
        public Builder WithLeader(bool v) { _isLeader = v; return this; }
        public Builder WithSelf(bool v) { _isSelf = v; return this; }

        public CommanderMember Build() => new(_uid, _name)
        {
            ProfessionId = _professionId,
            ProfessionName = _professionName,
            Level = _level,
            FightPoint = _fightPoint,
            Online = _online,
            IsLeader = _isLeader,
            IsSelf = _isSelf,
        };
    }
}
