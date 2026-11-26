#include "otf2_trace_reader.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QTimeZone>
#include <QUrl>
#include <QVector>

#include <otf2/otf2.h>

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <limits>
#include <optional>
#include <thread>
#include <variant>
#include <vector>
#include <cstdio>
#include <memory>
#include <mutex>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace trace {

QString resolveTracePath(const QString &urlOrPath, QString *error)
{
    QString path = urlOrPath;
    const QUrl url(urlOrPath);
    if (url.isLocalFile())
        path = url.toLocalFile();

    const QFileInfo info(path);
    if (info.isDir()) {
        const QString anchor = QDir(path).filePath(QStringLiteral("traces.otf2"));
        if (QFileInfo(anchor).isFile())
            return anchor;
        if (error)
            *error = QStringLiteral("No traces.otf2 found in %1").arg(path);
        return {};
    }
    if (!info.isFile()) {
        if (error)
            *error = QStringLiteral("File not found: %1").arg(path);
        return {};
    }
    return path;
}

namespace {

constexpr qint64 kNsPerSecond = 1'000'000'000;

// ---------------------------------------------------------------------------------------
// OTF2 error capture: OTF2 reports errors through a process-wide handler (default: print
// to stderr). While a read is running, the last message is captured for *error instead.
// Reads may run concurrently on several worker threads: the capturing handler (which writes
// to thread-local storage) is installed by the first active read and the previous handler
// restored by the last one, so overlapping reads never restore each other's state.

thread_local QString tLastOtf2Error;
thread_local OTF2_ErrorCode tLastOtf2Code = OTF2_SUCCESS;

OTF2_ErrorCode captureOtf2Error(void *, const char *, uint64_t, const char *, OTF2_ErrorCode code,
                                const char *fmt, va_list va)
{
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, va);
    tLastOtf2Code = code;
    tLastOtf2Error = QStringLiteral("%1 (%2)").arg(QString::fromUtf8(buf),
                                                   QString::fromUtf8(OTF2_Error_GetName(code)));
    return code;
}

class Otf2ErrorCapture
{
public:
    Otf2ErrorCapture()
    {
        tLastOtf2Error.clear();
        tLastOtf2Code = OTF2_SUCCESS;
        const std::lock_guard<std::mutex> lock(s_mutex);
        if (s_active++ == 0)
            s_previous = OTF2_Error_RegisterCallback(&captureOtf2Error, nullptr);
    }
    ~Otf2ErrorCapture()
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        if (--s_active == 0)
            OTF2_Error_RegisterCallback(s_previous, nullptr);
    }
    Otf2ErrorCapture(const Otf2ErrorCapture &) = delete;
    Otf2ErrorCapture &operator=(const Otf2ErrorCapture &) = delete;

    static QString message(OTF2_ErrorCode code)
    {
        return tLastOtf2Error.isEmpty() ? QString::fromUtf8(OTF2_Error_GetDescription(code))
                                        : tLastOtf2Error;
    }

private:
    static inline std::mutex s_mutex;
    static inline int s_active = 0;
    static inline OTF2_ErrorCallback s_previous = nullptr;
};

struct ReaderCloser {
    void operator()(OTF2_Reader *r) const { OTF2_Reader_Close(r); }
};
using ReaderPtr = std::unique_ptr<OTF2_Reader, ReaderCloser>;

// ---------------------------------------------------------------------------------------
// Global definitions, stored raw (definition order is not guaranteed to be dependency
// order) and resolved after all of them have been read.

struct RawNode {
    OTF2_StringRef name;
    OTF2_StringRef className;
    OTF2_SystemTreeNodeRef parent;
};
struct RawLocationGroup {
    OTF2_LocationGroupType type;
    OTF2_SystemTreeNodeRef parent;
};
struct RawLocation {
    OTF2_LocationType type;
    uint64_t events;
    OTF2_LocationGroupRef group;
};
struct RawGroup {
    OTF2_GroupType type;
    OTF2_Paradigm paradigm;
    OTF2_GroupFlag flags;
    QVector<uint64_t> members;
};
struct RawRegion {
    OTF2_StringRef name;
    OTF2_RegionRole role;
    OTF2_Paradigm paradigm;
};

struct GlobalDefs {
    uint64_t resolution = kNsPerSecond;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t realtime = OTF2_UNDEFINED_TIMESTAMP;
    QHash<OTF2_StringRef, QString> strings;
    QHash<OTF2_SystemTreeNodeRef, RawNode> nodes;
    QMap<OTF2_LocationGroupRef, RawLocationGroup> locationGroups;
    QMap<OTF2_LocationRef, RawLocation> locations;
    QHash<OTF2_GroupRef, RawGroup> groups;
    QHash<OTF2_CommRef, OTF2_GroupRef> comms;
    QHash<OTF2_CommRef, OTF2_StringRef> commNames;
    QHash<OTF2_RegionRef, RawRegion> regions;
    // Synchronous metric classes (e.g. PAPI counters, written as METRIC events at every
    // Enter / Leave) or metric instances (asynchronous recorders) exist. Metric members alone
    // (Score-P always defines bytes_sent etc.) produce no events.
    bool hasMetrics = false;
};

OTF2_CallbackCode onClock(void *ud, uint64_t resolution, uint64_t offset, uint64_t length, uint64_t realtime)
{
    auto *d = static_cast<GlobalDefs *>(ud);
    d->resolution = resolution ? resolution : kNsPerSecond;
    d->offset = offset;
    d->length = length;
    d->realtime = realtime;
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onString(void *ud, OTF2_StringRef self, const char *s)
{
    static_cast<GlobalDefs *>(ud)->strings.insert(self, QString::fromUtf8(s));
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onSystemTreeNode(void *ud, OTF2_SystemTreeNodeRef self, OTF2_StringRef name,
                                   OTF2_StringRef className, OTF2_SystemTreeNodeRef parent)
{
    static_cast<GlobalDefs *>(ud)->nodes.insert(self, {name, className, parent});
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onLocationGroup(void *ud, OTF2_LocationGroupRef self, OTF2_StringRef,
                                  OTF2_LocationGroupType type, OTF2_SystemTreeNodeRef parent,
                                  OTF2_LocationGroupRef)
{
    static_cast<GlobalDefs *>(ud)->locationGroups.insert(self, {type, parent});
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onLocation(void *ud, OTF2_LocationRef self, OTF2_StringRef, OTF2_LocationType type,
                             uint64_t events, OTF2_LocationGroupRef group)
{
    static_cast<GlobalDefs *>(ud)->locations.insert(self, {type, events, group});
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onGroup(void *ud, OTF2_GroupRef self, OTF2_StringRef, OTF2_GroupType type,
                          OTF2_Paradigm paradigm, OTF2_GroupFlag flags, uint32_t count,
                          const uint64_t *members)
{
    RawGroup g{type, paradigm, flags, {}};
    g.members.reserve(int(count));
    for (uint32_t i = 0; i < count; ++i)
        g.members.append(members[i]);
    static_cast<GlobalDefs *>(ud)->groups.insert(self, g);
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onComm(void *ud, OTF2_CommRef self, OTF2_StringRef name, OTF2_GroupRef group,
                         OTF2_CommRef, OTF2_CommFlag)
{
    auto *d = static_cast<GlobalDefs *>(ud);
    d->comms.insert(self, group);
    d->commNames.insert(self, name);
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onRegion(void *ud, OTF2_RegionRef self, OTF2_StringRef name, OTF2_StringRef,
                           OTF2_StringRef, OTF2_RegionRole role, OTF2_Paradigm paradigm, OTF2_RegionFlag,
                           OTF2_StringRef, uint32_t, uint32_t)
{
    static_cast<GlobalDefs *>(ud)->regions.insert(self, {name, role, paradigm});
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMetricClass(void *ud, OTF2_MetricRef, uint8_t numberOfMetrics, const OTF2_MetricMemberRef *,
                                OTF2_MetricOccurrence occurrence, OTF2_RecorderKind)
{
    if (numberOfMetrics > 0
        && (occurrence == OTF2_METRIC_SYNCHRONOUS_STRICT || occurrence == OTF2_METRIC_SYNCHRONOUS))
        static_cast<GlobalDefs *>(ud)->hasMetrics = true;
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMetricInstance(void *ud, OTF2_MetricRef, OTF2_MetricRef, OTF2_LocationRef,
                                   OTF2_MetricScope, uint64_t)
{
    static_cast<GlobalDefs *>(ud)->hasMetrics = true;
    return OTF2_CALLBACK_SUCCESS;
}

bool readGlobalDefs(OTF2_Reader *reader, GlobalDefs &defs, QString *error)
{
    OTF2_GlobalDefReader *gdr = OTF2_Reader_GetGlobalDefReader(reader);
    if (!gdr) {
        *error = QStringLiteral("Cannot read OTF2 global definitions: %1")
                     .arg(Otf2ErrorCapture::message(OTF2_ERROR_INVALID));
        return false;
    }
    OTF2_GlobalDefReaderCallbacks *cb = OTF2_GlobalDefReaderCallbacks_New();
    OTF2_GlobalDefReaderCallbacks_SetClockPropertiesCallback(cb, &onClock);
    OTF2_GlobalDefReaderCallbacks_SetStringCallback(cb, &onString);
    OTF2_GlobalDefReaderCallbacks_SetSystemTreeNodeCallback(cb, &onSystemTreeNode);
    OTF2_GlobalDefReaderCallbacks_SetLocationGroupCallback(cb, &onLocationGroup);
    OTF2_GlobalDefReaderCallbacks_SetLocationCallback(cb, &onLocation);
    OTF2_GlobalDefReaderCallbacks_SetGroupCallback(cb, &onGroup);
    OTF2_GlobalDefReaderCallbacks_SetCommCallback(cb, &onComm);
    OTF2_GlobalDefReaderCallbacks_SetRegionCallback(cb, &onRegion);
    OTF2_GlobalDefReaderCallbacks_SetMetricClassCallback(cb, &onMetricClass);
    OTF2_GlobalDefReaderCallbacks_SetMetricInstanceCallback(cb, &onMetricInstance);
    const OTF2_ErrorCode regRc = OTF2_Reader_RegisterGlobalDefCallbacks(reader, gdr, cb, &defs);
    OTF2_GlobalDefReaderCallbacks_Delete(cb);

    uint64_t read = 0;
    const OTF2_ErrorCode rc = regRc != OTF2_SUCCESS
        ? regRc
        : OTF2_Reader_ReadAllGlobalDefinitions(reader, gdr, &read);
    OTF2_Reader_CloseGlobalDefReader(reader, gdr);
    if (rc != OTF2_SUCCESS) {
        *error = QStringLiteral("Cannot read OTF2 global definitions: %1").arg(Otf2ErrorCapture::message(rc));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Resolved tables used while reading events.

struct RegionInfo {
    QString name;
    bool mpi = false;
    bool user = false; // user function: non-MPI, not artificial / measurement system
    CallKind kind = CallKind::OtherMpi;
};

struct CommInfo {
    bool self = false;        // MPI_COMM_SELF-like: every rank is 0, i.e. the location itself
    bool globalRanks = false; // event ranks are already world ranks
    QVector<int> members;     // world ranks, indexed by comm-local rank
};

} // namespace

// Reader state kept in checkpoints (ReadCheckpoint), hence outside the anonymous namespace.
namespace reader_state {

// One open MPI region of a location plus the records that belong to it.
struct OpenRegion {
    OTF2_RegionRef region;
    qint64 enterNs;
    QVector<P2PSend> sends;
    QVector<P2PRecv> recvs;
    bool hasCollective = false;
    Collective collective{};
};

// One open user region of a location.
struct OpenUserRegion {
    OTF2_RegionRef region;
    qint64 enterNs;
};

struct LocationState {
    int rank = -1;
    uint64_t delivered = 0; // events of the location delivered so far (all event types)
    QVector<OpenRegion> stack;
    QVector<OpenUserRegion> userStack;
};

} // namespace reader_state

struct ReadCheckpoint {
    qint64 ns = 0;
    OTF2_TimeStamp raw = 0;                      // timestamp of the first event not yet read
    QHash<OTF2_LocationRef, uint64_t> positions; // OTF2_EvtReader_Seek target per location
    QHash<OTF2_LocationRef, reader_state::LocationState> locations;
    qint64 maxNs = 0;
    std::shared_ptr<const MatchState> match;
};

int TraceIndex::indexBefore(qint64 ns) const
{
    return int(std::upper_bound(m_times.cbegin(), m_times.cend(), ns) - m_times.cbegin()) - 1;
}

std::shared_ptr<const ReadCheckpoint> TraceIndex::before(qint64 ns) const
{
    const int i = indexBefore(ns);
    return i >= 0 ? m_points[i] : nullptr;
}

void TraceIndex::clear()
{
    m_times.clear();
    m_points.clear();
}

void TraceIndex::append(qint64 ns, std::shared_ptr<const ReadCheckpoint> checkpoint)
{
    m_times.append(ns);
    m_points.append(std::move(checkpoint));
}

namespace {

using reader_state::LocationState;
using reader_state::OpenRegion;
using reader_state::OpenUserRegion;

QString nodeNameOf(const GlobalDefs &d, OTF2_SystemTreeNodeRef parent)
{
    const QString node = QStringLiteral("node");
    OTF2_SystemTreeNodeRef cur = parent;
    for (int depth = 0; depth < 64 && d.nodes.contains(cur); ++depth) {
        const RawNode &n = d.nodes[cur];
        if (d.strings.value(n.className) == node)
            return d.strings.value(n.name);
        cur = n.parent;
    }
    return d.nodes.contains(parent) ? d.strings.value(d.nodes[parent].name) : QString();
}

// World rank per location group. Prefers the MPI COMM_LOCATIONS (or COMM_LOCATION_GROUPS)
// group; without one, process location groups are numbered in definition-id order.
QMap<OTF2_LocationGroupRef, int> rankPerLocationGroup(const GlobalDefs &d)
{
    QMap<OTF2_LocationGroupRef, int> ranks;
    for (const RawGroup &g : d.groups) {
        if (g.paradigm != OTF2_PARADIGM_MPI)
            continue;
        if (g.type == OTF2_GROUP_TYPE_COMM_LOCATIONS) {
            for (int i = 0; i < g.members.size(); ++i) {
                const auto loc = d.locations.constFind(g.members[i]);
                if (loc != d.locations.cend() && !ranks.contains(loc->group))
                    ranks.insert(loc->group, i);
            }
            return ranks;
        }
        if (g.type == OTF2_GROUP_TYPE_COMM_LOCATION_GROUPS) {
            for (int i = 0; i < g.members.size(); ++i)
                ranks.insert(OTF2_LocationGroupRef(g.members[i]), i);
            return ranks;
        }
    }
    int next = 0;
    for (auto it = d.locationGroups.cbegin(); it != d.locationGroups.cend(); ++it)
        if (it->type == OTF2_LOCATION_GROUP_TYPE_PROCESS)
            ranks.insert(it.key(), next++);
    return ranks;
}

QHash<OTF2_RegionRef, RegionInfo> resolveRegions(const GlobalDefs &d)
{
    QHash<OTF2_RegionRef, RegionInfo> regions;
    for (auto it = d.regions.cbegin(); it != d.regions.cend(); ++it) {
        RegionInfo info;
        info.name = d.strings.value(it->name);
        info.mpi = it->paradigm == OTF2_PARADIGM_MPI;
        if (info.mpi)
            info.kind = classifyMpiFunction(info.name); // once per region definition
        // Score-P 10 marks "TRACE BUFFER FLUSH" / "MEASUREMENT OFF" etc. as ARTIFICIAL
        // (paradigm MEASUREMENT_SYSTEM or USER). Names starting with MPI_ are never user
        // functions, whatever paradigm an old writer gave them.
        info.user = !info.mpi && it->role != OTF2_REGION_ROLE_ARTIFICIAL
                 && it->paradigm != OTF2_PARADIGM_MEASUREMENT_SYSTEM
                 && !info.name.startsWith(QLatin1String("MPI_"));
        regions.insert(it.key(), info);
    }
    return regions;
}

QHash<OTF2_CommRef, CommInfo> resolveComms(const GlobalDefs &d)
{
    QHash<OTF2_CommRef, CommInfo> comms;
    for (auto it = d.comms.cbegin(); it != d.comms.cend(); ++it) {
        const auto g = d.groups.constFind(it.value());
        if (g == d.groups.cend() || g->paradigm != OTF2_PARADIGM_MPI)
            continue; // unknown / non-MPI communicator: its events are skipped
        CommInfo c;
        switch (g->type) {
        case OTF2_GROUP_TYPE_COMM_SELF:
            c.self = true;
            break;
        case OTF2_GROUP_TYPE_COMM_GROUP:
            // Members are indices into COMM_LOCATIONS, i.e. world ranks.
            for (uint64_t m : g->members)
                c.members.append(int(m));
            c.globalRanks = (g->flags & OTF2_GROUP_FLAG_GLOBAL_MEMBERS) != 0;
            break;
        case OTF2_GROUP_TYPE_COMM_LOCATIONS:
        case OTF2_GROUP_TYPE_COMM_LOCATION_GROUPS:
            for (int i = 0; i < g->members.size(); ++i)
                c.members.append(i);
            break;
        default:
            continue;
        }
        comms.insert(it.key(), c);
    }
    return comms;
}

// ---------------------------------------------------------------------------------------
// Event translation.

class EventTranslator
{
public:
    // An MPI communicator whose events are translated (resolveComms kept it).
    bool knowsComm(OTF2_CommRef comm) const { return m_comms.contains(comm); }

    EventTranslator(TraceSink &sink, const GlobalDefs &d, const std::atomic_bool *cancel)
        : m_sink(sink)
        , m_cancel(cancel)
        , m_resolution(d.resolution)
        , m_offset(d.offset)
        , m_regions(resolveRegions(d))
        , m_comms(resolveComms(d))
    {
    }

    void addLocation(OTF2_LocationRef loc, int rank) { m_locations[loc].rank = rank; }
    bool cancelled() const { return m_cancel && m_cancel->load(std::memory_order_relaxed); }

    // Early stop (TraceSink::stopAfterNs); stopAfterNs < 0 = never.
    void setStopAfter(qint64 stopAfterNs) { m_stopAfter = stopAfterNs; }
    bool stopped() const { return m_stopped; }
    // Called for every event before it is translated: true (the read ends here, the event is
    // not used) once past the stop time with no MPI call entered up to it still open and no
    // partner awaited by the sink. Checked every kStopCheckEvents events past the stop time.
    bool shouldStop(qint64 ns)
    {
        if (m_stopAfter < 0 || ns <= m_stopAfter || ++m_sinceStopCheck < kStopCheckEvents)
            return false;
        m_sinceStopCheck = 0;
        for (const LocationState &st : std::as_const(m_locations))
            for (const OpenRegion &r : st.stack)
                if (r.enterNs <= m_stopAfter)
                    return false;
        if (m_sink.waitingForPartners())
            return false;
        m_stopped = true;
        return true;
    }
    qint64 maxEventNs() const { return m_maxNs; }

    qint64 durationNs(uint64_t ticks) const
    {
        if (m_resolution == uint64_t(kNsPerSecond))
            return qint64(ticks);
        const uint64_t sec = ticks / m_resolution;
        const uint64_t rem = ticks % m_resolution;
        return qint64(sec) * kNsPerSecond
             + qint64(static_cast<long double>(rem) * kNsPerSecond / m_resolution);
    }

    qint64 nsOf(OTF2_TimeStamp t) const { return t <= m_offset ? 0 : durationNs(t - m_offset); }
    void noteTime(qint64 ns) { m_maxNs = std::max(m_maxNs, ns); }

    // Index building: checkpoints at count - 1 evenly spaced times over lengthNs.
    void setIndex(TraceIndex *index, const QList<OTF2_LocationRef> &locations, int count, qint64 lengthNs)
    {
        m_index = index;
        m_indexLocations = locations;
        m_cpTimes.clear();
        m_nextCp = 0;
        if (!index || count < 2 || lengthNs <= 0)
            return;
        for (int k = 1; k < count; ++k)
            m_cpTimes.append(qint64(double(lengthNs) * k / count));
    }
    // Called for every event before it is translated (its time ns / raw).
    void maybeCheckpoint(OTF2_LocationRef current, qint64 ns, OTF2_TimeStamp raw)
    {
        if (m_nextCp >= m_cpTimes.size() || ns < m_cpTimes[m_nextCp])
            return;
        while (m_nextCp < m_cpTimes.size() && ns >= m_cpTimes[m_nextCp])
            ++m_nextCp;
        auto cp = std::make_shared<ReadCheckpoint>();
        cp->ns = ns;
        cp->raw = raw;
        // Event positions start at 1 and Seek(p) makes p the next event read. Resume every
        // location at its first event not yet delivered (the event being delivered now is not
        // counted yet). OTF2_EvtReader_GetPos is no help here: the local readers read ahead by
        // a varying number of events.
        Q_UNUSED(current);
        cp->positions.reserve(m_indexLocations.size());
        for (OTF2_LocationRef loc : std::as_const(m_indexLocations))
            cp->positions.insert(loc, m_locations.value(loc).delivered + 1);
        cp->locations = m_locations;
        cp->maxNs = m_maxNs;
        m_sink.saveMatchStateInto(&cp->match);
        m_index->append(ns, std::move(cp));
    }

    // Resuming at a checkpoint: its reader state and the sink's matching state; events before
    // its time (only the last event of locations already exhausted there) are skipped.
    void resume(const ReadCheckpoint &cp)
    {
        m_locations = cp.locations;
        m_maxNs = cp.maxNs;
        m_resumeRaw = cp.raw;
        m_sink.restoreMatchState(cp.match);
    }
    bool beforeResume(OTF2_TimeStamp t) const { return t < m_resumeRaw; }

    LocationState *location(OTF2_LocationRef loc)
    {
        const auto it = m_locations.find(loc);
        return it == m_locations.end() ? nullptr : &it.value();
    }

    void enter(LocationState &st, OTF2_RegionRef region, qint64 ns)
    {
        const RegionInfo *info = regionInfo(region);
        if (!info)
            return;
        if (info->mpi)
            st.stack.append({region, ns, {}, {}, false, {}});
        else if (info->user)
            st.userStack.append({region, ns});
    }

    void leave(LocationState &st, OTF2_RegionRef region, qint64 ns)
    {
        const RegionInfo *info = regionInfo(region);
        if (!info)
            return;
        if (info->user) {
            for (int i = int(st.userStack.size()) - 1; i >= 0; --i) {
                if (st.userStack[i].region != region)
                    continue;
                // Unmatched user regions above it (malformed trace) are closed here as well.
                while (st.userStack.size() > i)
                    emitUserRegion(st, ns);
                return;
            }
            return;
        }
        if (!info->mpi)
            return;
        for (int i = int(st.stack.size()) - 1; i >= 0; --i) {
            if (st.stack[i].region != region)
                continue;
            // Unmatched MPI regions above it (malformed trace) are closed here as well.
            while (st.stack.size() > i)
                emitRegion(st, st.stack.takeLast(), ns);
            return;
        }
    }

    void send(LocationState &st, OTF2_CommRef comm, uint32_t receiver, uint32_t tag, uint64_t bytes,
              qint64 ns, const char *fallbackFn)
    {
        const int peer = worldRank(comm, receiver, st.rank);
        if (peer < 0)
            return;
        P2PSend s{st.rank, peer, comm, tag, bytes, {}, ns, ns};
        if (st.stack.isEmpty()) {
            s.function = QString::fromLatin1(fallbackFn);
            m_sink.addSend(s);
        } else {
            st.stack.last().sends.append(s);
        }
    }

    // fixedFn: function name of the record regardless of the enclosing region (used for
    // MpiIrecv completions, which lie inside MPI_Wait* / MPI_Test*); otherwise the record is
    // named after its enclosing MPI region, or fallbackFn outside of any. The record always
    // takes the times of the enclosing region.
    void recv(LocationState &st, OTF2_CommRef comm, uint32_t sender, uint32_t tag, uint64_t bytes,
              qint64 ns, const char *fallbackFn, const char *fixedFn = nullptr)
    {
        const int peer = worldRank(comm, sender, st.rank);
        if (peer < 0)
            return;
        P2PRecv r{st.rank, peer, comm, tag, bytes, {}, ns, ns};
        if (fixedFn)
            r.function = QString::fromLatin1(fixedFn);
        if (st.stack.isEmpty()) {
            if (!fixedFn)
                r.function = QString::fromLatin1(fallbackFn);
            m_sink.addRecv(r);
        } else {
            st.stack.last().recvs.append(r);
        }
    }

    void collectiveEnd(LocationState &st, OTF2_CommRef comm, uint64_t sent, uint64_t received)
    {
        const auto c = m_comms.constFind(comm);
        if (c == m_comms.cend() || st.stack.isEmpty())
            return;
        OpenRegion &top = st.stack.last();
        top.hasCollective = true;
        top.collective.rank = st.rank;
        top.collective.comm = comm;
        top.collective.members = c->self ? QVector<int>{st.rank} : c->members;
        top.collective.bytesSent = sent;
        top.collective.bytesRecv = received;
    }

    // Regions still open at the end of the trace are closed at endNs.
    void closeAll(qint64 endNs)
    {
        if (m_stopped) {
            // Regions entered after the stop time are not needed (and their real end unknown).
            for (LocationState &st : m_locations) {
                st.stack.clear();
                while (!st.userStack.isEmpty() && st.userStack.last().enterNs > m_stopAfter)
                    st.userStack.removeLast();
            }
        }
        for (LocationState &st : m_locations) {
            while (!st.stack.isEmpty()) {
                const OpenRegion r = st.stack.takeLast();
                emitRegion(st, r, std::max(endNs, r.enterNs));
            }
            while (!st.userStack.isEmpty())
                emitUserRegion(st, std::max(endNs, st.userStack.last().enterNs));
        }
    }

private:
    const RegionInfo *regionInfo(OTF2_RegionRef region) const
    {
        const auto it = m_regions.constFind(region);
        return it == m_regions.cend() ? nullptr : &it.value();
    }

    // Pops the innermost open user region; its depth is the number of user regions below it.
    void emitUserRegion(LocationState &st, qint64 leaveNs)
    {
        const OpenUserRegion u = st.userStack.takeLast();
        m_sink.addUserRegion({st.rank, m_regions[u.region].name, int(st.userStack.size()), u.enterNs, leaveNs});
    }

    int worldRank(OTF2_CommRef comm, uint32_t local, int self) const
    {
        const auto c = m_comms.constFind(comm);
        if (c == m_comms.cend())
            return -1;
        if (c->self)
            return local == 0 ? self : -1;
        if (c->globalRanks)
            return int(local);
        return local < uint32_t(c->members.size()) ? c->members[int(local)] : -1;
    }

    void emitRegion(const LocationState &st, const OpenRegion &r, qint64 leaveNs)
    {
        const RegionInfo &info = m_regions[r.region];
        m_sink.addCall({st.rank, info.kind, info.name, r.enterNs, leaveNs});
        for (P2PSend s : r.sends) {
            s.function = info.name;
            s.enterNs = r.enterNs;
            s.leaveNs = leaveNs;
            m_sink.addSend(s);
        }
        for (P2PRecv rv : r.recvs) {
            if (rv.function.isEmpty())
                rv.function = info.name;
            rv.enterNs = r.enterNs;
            rv.leaveNs = leaveNs;
            m_sink.addRecv(rv);
        }
        // Collective records only for collective operations; handle creation etc. in
        // MPI_Init / MPI_Comm_* also carries MpiCollectiveEnd but is not communication.
        if (r.hasCollective && info.kind == CallKind::Collective) {
            Collective c = r.collective;
            c.function = info.name;
            c.enterNs = r.enterNs;
            c.leaveNs = leaveNs;
            m_sink.addCollective(c);
        }
    }

    TraceSink &m_sink;
    const std::atomic_bool *m_cancel;
    uint64_t m_resolution;
    uint64_t m_offset;
    QHash<OTF2_RegionRef, RegionInfo> m_regions;
    QHash<OTF2_CommRef, CommInfo> m_comms;
    QHash<OTF2_LocationRef, LocationState> m_locations;
    qint64 m_maxNs = 0;
    static constexpr int kStopCheckEvents = 4096;
    TraceIndex *m_index = nullptr;
    QList<OTF2_LocationRef> m_indexLocations;
    QVector<qint64> m_cpTimes;
    int m_nextCp = 0;
    OTF2_TimeStamp m_resumeRaw = 0;
    qint64 m_stopAfter = -1;
    int m_sinceStopCheck = kStopCheckEvents - 1; // first event past the stop time checks
    bool m_stopped = false;
};

// Common prologue of all event callbacks: cancellation check and location lookup.
#define EVT_PROLOGUE                                                       \
    auto *tr = static_cast<EventTranslator *>(ud);                         \
    if (tr->cancelled())                                                   \
        return OTF2_CALLBACK_INTERRUPT;                                    \
    const qint64 ns = tr->nsOf(time);                                      \
    /* Checkpoint first: it copies the location states, and st below must */\
    /* point into the detached copy the read goes on with, not the saved one. */\
    tr->maybeCheckpoint(loc, ns, time);                                    \
    LocationState *st = tr->location(loc);                                 \
    if (!st)                                                               \
        return OTF2_CALLBACK_SUCCESS;                                      \
    ++st->delivered;                                                       \
    if (tr->beforeResume(time))                                            \
        return OTF2_CALLBACK_SUCCESS;                                      \
    tr->noteTime(ns);                                                      \
    if (tr->shouldStop(ns))                                                \
        return OTF2_CALLBACK_INTERRUPT

OTF2_CallbackCode onEnter(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                          OTF2_RegionRef region)
{
    EVT_PROLOGUE;
    tr->enter(*st, region, ns);
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onLeave(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                          OTF2_RegionRef region)
{
    EVT_PROLOGUE;
    tr->leave(*st, region, ns);
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMpiSend(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                            uint32_t receiver, OTF2_CommRef comm, uint32_t tag, uint64_t bytes)
{
    EVT_PROLOGUE;
    tr->send(*st, comm, receiver, tag, bytes, ns, "MPI_Send");
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMpiIsend(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                             uint32_t receiver, OTF2_CommRef comm, uint32_t tag, uint64_t bytes, uint64_t)
{
    EVT_PROLOGUE;
    tr->send(*st, comm, receiver, tag, bytes, ns, "MPI_Isend");
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMpiRecv(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                            uint32_t sender, OTF2_CommRef comm, uint32_t tag, uint64_t bytes)
{
    EVT_PROLOGUE;
    tr->recv(*st, comm, sender, tag, bytes, ns, "MPI_Recv");
    return OTF2_CALLBACK_SUCCESS;
}

// Completion of an MPI_Irecv; occurs inside the completing MPI_Wait* / MPI_Test* region.
// Booked as "MPI_Irecv" (like EduMPI's DB rows) with the times of the completing region.
OTF2_CallbackCode onMpiIrecv(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                             uint32_t sender, OTF2_CommRef comm, uint32_t tag, uint64_t bytes, uint64_t)
{
    EVT_PROLOGUE;
    tr->recv(*st, comm, sender, tag, bytes, ns, "MPI_Irecv", "MPI_Irecv");
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onMpiCollectiveEnd(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud,
                                     OTF2_AttributeList *, OTF2_CollectiveOp, OTF2_CommRef comm,
                                     uint32_t, uint64_t sent, uint64_t received)
{
    EVT_PROLOGUE;
    Q_UNUSED(ns);
    tr->collectiveEnd(*st, comm, sent, received);
    return OTF2_CALLBACK_SUCCESS;
}

#undef EVT_PROLOGUE

// Events not translated are still counted per location (checkpoint positions). One counting
// callback per OTF2 event type, instantiated from the setter's callback type: all global event
// callbacks start with (location, time, userData, attributeList).
template <typename Fn>
struct EventCounter;
template <typename... Args>
struct EventCounter<OTF2_CallbackCode (*)(OTF2_LocationRef, OTF2_TimeStamp, void *, OTF2_AttributeList *, Args...)> {
    static OTF2_CallbackCode count(OTF2_LocationRef loc, OTF2_TimeStamp, void *ud, OTF2_AttributeList *, Args...)
    {
        auto *tr = static_cast<EventTranslator *>(ud);
        if (tr->cancelled())
            return OTF2_CALLBACK_INTERRUPT;
        if (LocationState *st = tr->location(loc))
            ++st->delivered;
        return OTF2_CALLBACK_SUCCESS;
    }
};
template <typename Fn>
void countWith(OTF2_ErrorCode (*set)(OTF2_GlobalEvtReaderCallbacks *, Fn), OTF2_GlobalEvtReaderCallbacks *cb)
{
    set(cb, &EventCounter<Fn>::count);
}

// OTF2 keeps one event file per location open while reading. With more locations than the
// process may open files (macOS: 256 for apps started from the Finder, Linux: often 1024), the
// read fails with EMFILE: say so instead of calling the trace damaged.
QString tooManyOpenFiles(qsizetype locations)
{
    QString limit = QStringLiteral("unknown");
#ifdef Q_OS_UNIX
    struct rlimit rl {};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        limit = QString::number(quint64(rl.rlim_cur));
#endif
    return QStringLiteral("Too many open files: the trace has %1 locations (one event file each), "
                          "but this process may only open %2 files at once. Raise the limit, e.g. "
                          "start the GUI from a shell after `ulimit -n 10240`.")
        .arg(locations)
        .arg(limit);
}

bool lastErrorWasTooManyFiles()
{
    return tLastOtf2Code == OTF2_ERROR_EMFILE || tLastOtf2Code == OTF2_ERROR_ENFILE;
}

QString incompleteTrace(const char *what, OTF2_LocationRef loc, OTF2_ErrorCode rc)
{
    return QStringLiteral("Trace is incomplete or damaged: could not read %1 of location %2 — %3")
        .arg(QLatin1String(what))
        .arg(loc)
        .arg(Otf2ErrorCapture::message(rc));
}

// Selects the locations, reads their local definitions (mapping tables, clock offsets)
// and opens their event readers. Missing local definition files are tolerated; local
// definitions that exist but cannot be read, or a missing event reader, fail the load
// (the mapping tables would be missing, i.e. names / partners silently wrong).
bool prepareLocations(OTF2_Reader *reader, const QList<OTF2_LocationRef> &locations, QString *error)
{
    for (OTF2_LocationRef loc : locations)
        OTF2_Reader_SelectLocation(reader, loc);

    const bool haveDefFiles = OTF2_Reader_OpenDefFiles(reader) == OTF2_SUCCESS;
    const OTF2_ErrorCode rc = OTF2_Reader_OpenEvtFiles(reader);
    if (rc != OTF2_SUCCESS) {
        *error = lastErrorWasTooManyFiles()
            ? tooManyOpenFiles(locations.size())
            : QStringLiteral("Cannot open OTF2 event files: %1").arg(Otf2ErrorCapture::message(rc));
        return false;
    }
    for (OTF2_LocationRef loc : locations) {
        if (haveDefFiles) {
            if (OTF2_DefReader *dr = OTF2_Reader_GetDefReader(reader, loc)) {
                uint64_t read = 0;
                const OTF2_ErrorCode drc = OTF2_Reader_ReadAllLocalDefinitions(reader, dr, &read);
                OTF2_Reader_CloseDefReader(reader, dr);
                if (drc != OTF2_SUCCESS) {
                    *error = incompleteTrace("definitions", loc, drc);
                    OTF2_Reader_CloseDefFiles(reader);
                    return false;
                }
            }
        }
        if (!OTF2_Reader_GetEvtReader(reader, loc)) {
            *error = lastErrorWasTooManyFiles() ? tooManyOpenFiles(locations.size())
                                                : incompleteTrace("events", loc, OTF2_ERROR_INVALID);
            if (haveDefFiles)
                OTF2_Reader_CloseDefFiles(reader);
            return false;
        }
    }
    if (haveDefFiles)
        OTF2_Reader_CloseDefFiles(reader);
    return true;
}

OTF2_ErrorCode readEvents(OTF2_Reader *reader, EventTranslator &tr, quint64 total, const ReadProgress &progress)
{
    OTF2_GlobalEvtReader *ger = OTF2_Reader_GetGlobalEvtReader(reader);
    if (!ger)
        return OTF2_ERROR_INVALID;
    OTF2_GlobalEvtReaderCallbacks *cb = OTF2_GlobalEvtReaderCallbacks_New();
    // All event types first (counting only), then the translated ones.
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetUnknownCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetBufferFlushCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMeasurementOnOffCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiIsendCompleteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiIrecvRequestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiRequestTestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiRequestCancelledCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiCollectiveBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpForkCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpJoinCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpAcquireLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpReleaseLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpTaskCreateCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpTaskSwitchCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetOmpTaskCompleteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMetricCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetParameterStringCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetParameterIntCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetParameterUnsignedIntCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaWinCreateCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaWinDestroyCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaCollectiveBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaCollectiveEndCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaGroupSyncCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaRequestLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaAcquireLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaTryLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaReleaseLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaSyncCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaWaitChangeCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaPutCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaGetCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaAtomicCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaOpCompleteBlockingCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaOpCompleteNonBlockingCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaOpTestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetRmaOpCompleteRemoteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadForkCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadJoinCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadTeamBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadTeamEndCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadAcquireLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadReleaseLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadTaskCreateCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadTaskSwitchCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadTaskCompleteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadCreateCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadWaitCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetThreadEndCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetCallingContextEnterCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetCallingContextLeaveCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetCallingContextSampleCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoCreateHandleCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoDestroyHandleCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoDuplicateHandleCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoSeekCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoChangeStatusFlagsCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoDeleteFileCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoOperationBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoOperationTestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoOperationIssuedCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoOperationCompleteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoOperationCancelledCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoAcquireLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoReleaseLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetIoTryLockCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetProgramBeginCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetProgramEndCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetNonBlockingCollectiveRequestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetNonBlockingCollectiveCompleteCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetCommCreateCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetCommDestroyCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiProbeCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiMrecvCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiImrecvRequestCallback, cb);
    countWith(&OTF2_GlobalEvtReaderCallbacks_SetMpiImrecvCallback, cb);
    OTF2_GlobalEvtReaderCallbacks_SetEnterCallback(cb, &onEnter);
    OTF2_GlobalEvtReaderCallbacks_SetLeaveCallback(cb, &onLeave);
    OTF2_GlobalEvtReaderCallbacks_SetMpiSendCallback(cb, &onMpiSend);
    OTF2_GlobalEvtReaderCallbacks_SetMpiIsendCallback(cb, &onMpiIsend);
    OTF2_GlobalEvtReaderCallbacks_SetMpiRecvCallback(cb, &onMpiRecv);
    OTF2_GlobalEvtReaderCallbacks_SetMpiIrecvCallback(cb, &onMpiIrecv);
    OTF2_GlobalEvtReaderCallbacks_SetMpiCollectiveEndCallback(cb, &onMpiCollectiveEnd);
    const OTF2_ErrorCode regRc = OTF2_Reader_RegisterGlobalEvtCallbacks(reader, ger, cb, &tr);
    OTF2_GlobalEvtReaderCallbacks_Delete(cb);

    // Read in chunks of about 0.5 % of the events to report progress in between.
    OTF2_ErrorCode rc = regRc;
    if (rc == OTF2_SUCCESS) {
        const uint64_t chunk = std::clamp<quint64>(total / 200, 1000, quint64(1) << 20);
        quint64 done = 0;
        for (;;) {
            uint64_t read = 0;
            rc = OTF2_Reader_ReadGlobalEvents(reader, ger, chunk, &read);
            done += read;
            if (rc != OTF2_SUCCESS || tr.cancelled() || tr.stopped())
                break;
            const bool finished = read < chunk;
            if (progress)
                progress(finished ? std::max(done, total) : std::min(done, total), total);
            if (finished)
                break;
        }
    }
    if (tr.stopped() && !tr.cancelled()) {
        rc = OTF2_SUCCESS; // interrupted by the early stop: a complete read of what is needed
        if (progress)
            progress(total, total);
    }
    OTF2_Reader_CloseGlobalEvtReader(reader, ger);
    OTF2_Reader_CloseEvtFiles(reader);
    return rc;
}

// Pipelined reads: the reader thread's EventTranslator writes into a RecordPipe, which hands
// the records in batches through a bounded queue to the calling thread (drainInto).
class RecordPipe : public TraceSink
{
public:
    explicit RecordPipe(const std::atomic_bool *cancel) : m_cancel(cancel) { m_batch.reserve(kBatch); }

    // Producer side (reader thread).
    void addRank(int, const QString &) override {} // ranks go to the sink directly
    void addCall(const MpiCall &call) override { put(call); }
    void addSend(const P2PSend &send) override { put(send); }
    void addRecv(const P2PRecv &recv) override { put(recv); }
    void addCollective(const Collective &coll) override { put(coll); }
    void addUserRegion(const UserRegion &region) override { put(region); }
    void saveMatchStateInto(std::shared_ptr<const MatchState> *target) override { put(SaveMatch{target}); }
    void progress(quint64 done, quint64 total)
    {
        put(Progress{done, total});
        flush(); // progress reaches the sink without waiting for a full batch
    }
    void close()
    {
        flush();
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
    }

    // Consumer side (calling thread): until close(); records are dropped once *cancel is set.
    void drainInto(TraceSink &sink, const ReadProgress &progress)
    {
        for (;;) {
            std::vector<Record> batch;
            {
                std::unique_lock lock(m_mutex);
                m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_closed; });
                if (m_queue.empty())
                    return;
                batch = std::move(m_queue.front());
                m_queue.pop_front();
                m_notFull.notify_one();
            }
            if (m_cancel && m_cancel->load(std::memory_order_relaxed))
                continue;
            for (Record &r : batch) {
                std::visit([&](auto &rec) { deliver(sink, progress, rec); }, r);
            }
        }
    }

private:
    struct SaveMatch {
        std::shared_ptr<const MatchState> *target;
    };
    struct Progress {
        quint64 done;
        quint64 total;
    };
    using Record = std::variant<MpiCall, P2PSend, P2PRecv, Collective, UserRegion, SaveMatch, Progress>;
    static constexpr size_t kBatch = 16384;
    static constexpr size_t kMaxBatches = 8; // bounds the memory of records in flight

    template <typename T>
    void put(T &&record)
    {
        m_batch.emplace_back(std::forward<T>(record));
        if (m_batch.size() >= kBatch)
            flush();
    }
    void flush()
    {
        if (m_batch.empty())
            return;
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < kMaxBatches; });
        m_queue.push_back(std::move(m_batch));
        m_batch = {};
        m_batch.reserve(kBatch);
        m_notEmpty.notify_one();
    }
    static void deliver(TraceSink &s, const ReadProgress &, const MpiCall &r) { s.addCall(r); }
    static void deliver(TraceSink &s, const ReadProgress &, const P2PSend &r) { s.addSend(r); }
    static void deliver(TraceSink &s, const ReadProgress &, const P2PRecv &r) { s.addRecv(r); }
    static void deliver(TraceSink &s, const ReadProgress &, const Collective &r) { s.addCollective(r); }
    static void deliver(TraceSink &s, const ReadProgress &, const UserRegion &r) { s.addUserRegion(r); }
    static void deliver(TraceSink &s, const ReadProgress &, const SaveMatch &r) { s.saveMatchStateInto(r.target); }
    static void deliver(TraceSink &, const ReadProgress &p, const Progress &r)
    {
        if (p)
            p(r.done, r.total);
    }

    const std::atomic_bool *m_cancel;
    std::vector<Record> m_batch;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<std::vector<Record>> m_queue;
    bool m_closed = false;
};

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

} // namespace

bool readOtf2(const QString &anchorFile, TraceSink &sink, TraceMeta *meta, QString *error,
              const std::atomic_bool *cancel, const ReadProgress &progress)
{
    ReadOptions options;
    options.cancel = cancel;
    options.progress = progress;
    return readOtf2(anchorFile, sink, meta, error, options);
}

bool readOtf2(const QString &anchorFile, TraceSink &sink, TraceMeta *meta, QString *error,
              const ReadOptions &options)
{
    const std::atomic_bool *cancel = options.cancel;
    const QFileInfo anchor(anchorFile);
    if (!anchor.isFile())
        return fail(error, QStringLiteral("File not found: %1").arg(anchorFile));
    if (cancel && cancel->load())
        return fail(error, QStringLiteral("Cancelled"));

    Otf2ErrorCapture capture;
    QString err;
    ReaderPtr reader(OTF2_Reader_Open(QFile::encodeName(anchor.absoluteFilePath()).constData()));
    if (!reader)
        return fail(error, QStringLiteral("Not a readable OTF2 trace: %1 — %2")
                               .arg(anchorFile, Otf2ErrorCapture::message(OTF2_ERROR_INVALID)));
    if (const OTF2_ErrorCode rc = OTF2_Reader_SetSerialCollectiveCallbacks(reader.get()); rc != OTF2_SUCCESS)
        return fail(error, QStringLiteral("Cannot initialise OTF2 reader: %1").arg(Otf2ErrorCapture::message(rc)));

    GlobalDefs defs;
    if (!readGlobalDefs(reader.get(), defs, &err))
        return fail(error, err);

    // Ranks and their nodes, then the CPU-thread locations belonging to them.
    const QMap<OTF2_LocationGroupRef, int> ranks = rankPerLocationGroup(defs);
    if (ranks.isEmpty())
        return fail(error, QStringLiteral("No MPI processes found in %1").arg(anchorFile));
    for (auto it = ranks.cbegin(); it != ranks.cend(); ++it)
        sink.addRank(it.value(), nodeNameOf(defs, defs.locationGroups.value(it.key()).parent));

    // Pipelined: only whole-trace reads (no resume, no early stop).
    const qint64 stopAfter = sink.stopAfterNs();
    const bool pipelined = options.pipelined && !options.resumeFrom
        && (stopAfter < 0 || stopAfter == std::numeric_limits<qint64>::max());
    std::optional<RecordPipe> pipe;
    if (pipelined)
        pipe.emplace(cancel);

    EventTranslator tr(pipe ? static_cast<TraceSink &>(*pipe) : sink, defs, cancel);
    QList<OTF2_LocationRef> selected;
    TraceMeta m;
    for (auto it = defs.locations.cbegin(); it != defs.locations.cend(); ++it) {
        if (it->type != OTF2_LOCATION_TYPE_CPU_THREAD || it->events == 0 || !ranks.contains(it->group))
            continue;
        selected.append(it.key());
        tr.addLocation(it.key(), ranks.value(it->group));
        m.eventCount += it->events;
    }

    m.name = anchor.absoluteDir().dirName();
    m.traceLengthNs = tr.durationNs(defs.length); // declared length; extended to the last event below
    m.hasMetrics = defs.hasMetrics;
    for (auto it = defs.commNames.cbegin(); it != defs.commNames.cend(); ++it) {
        if (!tr.knowsComm(it.key()))
            continue; // non-MPI communicator: its events are skipped
        const QString name = defs.strings.value(it.value());
        m.commNames.insert(it.key(), name.isEmpty() ? QStringLiteral("Comm %1").arg(it.key()) : name);
    }
    m.realtimeStart = defs.realtime == OTF2_UNDEFINED_TIMESTAMP
        ? QDateTime()
        : QDateTime::fromMSecsSinceEpoch(qint64(defs.realtime / 1'000'000), QTimeZone::UTC);
    sink.begin(m);
    tr.setStopAfter(pipelined ? -1 : stopAfter);

    // Resuming: locations that delivered all their events before the checkpoint are not read.
    QList<OTF2_LocationRef> toRead = selected;
    const ReadCheckpoint *resume = options.resumeFrom.get();
    quint64 total = m.eventCount;
    if (resume) {
        toRead.clear();
        for (OTF2_LocationRef loc : std::as_const(selected)) {
            const uint64_t next = resume->positions.value(loc, 1);
            total -= std::min<quint64>(total, next - 1);
            if (next <= defs.locations.value(loc).events)
                toRead.append(loc);
        }
        tr.resume(*resume);
    }

    // The event part: on the calling thread, or (pipelined) on a worker thread whose records the
    // calling thread passes on to the sink meanwhile. Error texts are made on the reading thread
    // (Otf2ErrorCapture is per thread).
    const auto readPart = [&](const ReadProgress &progress, QString *readError) -> bool {
        if (!toRead.isEmpty()) {
            if (!prepareLocations(reader.get(), toRead, readError))
                return false;
            if (resume) {
                for (OTF2_LocationRef loc : std::as_const(toRead)) {
                    const uint64_t next = resume->positions.value(loc, 1);
                    if (next <= 1)
                        continue;
                    OTF2_EvtReader *er = OTF2_Reader_GetEvtReader(reader.get(), loc);
                    if (!er || OTF2_EvtReader_Seek(er, next) != OTF2_SUCCESS) {
                        *readError = QStringLiteral("Cannot seek in the trace (location %1)").arg(loc);
                        return false;
                    }
                }
            }
            if (options.buildIndex) {
                options.buildIndex->clear();
                tr.setIndex(options.buildIndex, selected, options.indexCheckpoints, m.traceLengthNs);
            }
            const OTF2_ErrorCode rc = readEvents(reader.get(), tr, total, progress);
            if (tr.cancelled()) {
                *readError = QStringLiteral("Cancelled");
                return false;
            }
            if (rc != OTF2_SUCCESS) {
                *readError = QStringLiteral("Cannot read OTF2 events: %1").arg(Otf2ErrorCapture::message(rc));
                return false;
            }
        }
        m.traceLengthNs = std::max(m.traceLengthNs, tr.maxEventNs());
        tr.closeAll(m.traceLengthNs);
        return true;
    };

    bool ok = false;
    if (pipe) {
        std::thread worker([&] {
            Otf2ErrorCapture workerCapture;
            ok = readPart([&](quint64 done, quint64 all) { pipe->progress(done, all); }, &err);
            pipe->close();
        });
        pipe->drainInto(sink, options.progress);
        worker.join();
    } else {
        ok = readPart(options.progress, &err);
    }
    if (!ok)
        return fail(error, err);

    if (meta)
        *meta = m;
    return true;
}

} // namespace trace
