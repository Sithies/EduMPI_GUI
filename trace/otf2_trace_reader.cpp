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
#include <cstdarg>
#include <cstdio>
#include <memory>

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

thread_local QString tLastOtf2Error;

OTF2_ErrorCode captureOtf2Error(void *, const char *, uint64_t, const char *, OTF2_ErrorCode code,
                                const char *fmt, va_list va)
{
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, va);
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
        m_previous = OTF2_Error_RegisterCallback(&captureOtf2Error, nullptr);
    }
    ~Otf2ErrorCapture() { OTF2_Error_RegisterCallback(m_previous, nullptr); }
    Otf2ErrorCapture(const Otf2ErrorCapture &) = delete;
    Otf2ErrorCapture &operator=(const Otf2ErrorCapture &) = delete;

    static QString message(OTF2_ErrorCode code)
    {
        return tLastOtf2Error.isEmpty() ? QString::fromUtf8(OTF2_Error_GetDescription(code))
                                        : tLastOtf2Error;
    }

private:
    OTF2_ErrorCallback m_previous;
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
    QHash<OTF2_RegionRef, RawRegion> regions;
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

OTF2_CallbackCode onComm(void *ud, OTF2_CommRef self, OTF2_StringRef, OTF2_GroupRef group,
                         OTF2_CommRef, OTF2_CommFlag)
{
    static_cast<GlobalDefs *>(ud)->comms.insert(self, group);
    return OTF2_CALLBACK_SUCCESS;
}

OTF2_CallbackCode onRegion(void *ud, OTF2_RegionRef self, OTF2_StringRef name, OTF2_StringRef,
                           OTF2_StringRef, OTF2_RegionRole, OTF2_Paradigm paradigm, OTF2_RegionFlag,
                           OTF2_StringRef, uint32_t, uint32_t)
{
    static_cast<GlobalDefs *>(ud)->regions.insert(self, {name, paradigm});
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
    OTF2_Reader_RegisterGlobalDefCallbacks(reader, gdr, cb, &defs);
    OTF2_GlobalDefReaderCallbacks_Delete(cb);

    uint64_t read = 0;
    const OTF2_ErrorCode rc = OTF2_Reader_ReadAllGlobalDefinitions(reader, gdr, &read);
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
    CallKind kind = CallKind::OtherMpi;
};

struct CommInfo {
    bool self = false;        // MPI_COMM_SELF-like: every rank is 0, i.e. the location itself
    bool globalRanks = false; // event ranks are already world ranks
    QVector<int> members;     // world ranks, indexed by comm-local rank
};

// One open MPI region of a location plus the records that belong to it.
struct OpenRegion {
    OTF2_RegionRef region;
    qint64 enterNs;
    QVector<P2PSend> sends;
    QVector<P2PRecv> recvs;
    bool hasCollective = false;
    Collective collective{};
};

struct LocationState {
    int rank = -1;
    QVector<OpenRegion> stack;
};

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
    EventTranslator(TraceAggregator &agg, const GlobalDefs &d, const std::atomic_bool *cancel)
        : m_agg(agg)
        , m_cancel(cancel)
        , m_resolution(d.resolution)
        , m_offset(d.offset)
        , m_regions(resolveRegions(d))
        , m_comms(resolveComms(d))
    {
    }

    void addLocation(OTF2_LocationRef loc, int rank) { m_locations[loc].rank = rank; }
    bool cancelled() const { return m_cancel && m_cancel->load(std::memory_order_relaxed); }
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

    qint64 timeNs(OTF2_TimeStamp t)
    {
        const qint64 ns = t <= m_offset ? 0 : durationNs(t - m_offset);
        m_maxNs = std::max(m_maxNs, ns);
        return ns;
    }

    LocationState *location(OTF2_LocationRef loc)
    {
        const auto it = m_locations.find(loc);
        return it == m_locations.end() ? nullptr : &it.value();
    }

    void enter(LocationState &st, OTF2_RegionRef region, qint64 ns)
    {
        if (isMpi(region))
            st.stack.append({region, ns, {}, {}, false, {}});
    }

    void leave(LocationState &st, OTF2_RegionRef region, qint64 ns)
    {
        if (!isMpi(region))
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
            m_agg.addSend(s);
        } else {
            st.stack.last().sends.append(s);
        }
    }

    void recv(LocationState &st, OTF2_CommRef comm, uint32_t sender, uint32_t tag, uint64_t bytes,
              qint64 ns, const char *fallbackFn)
    {
        const int peer = worldRank(comm, sender, st.rank);
        if (peer < 0)
            return;
        P2PRecv r{st.rank, peer, comm, tag, bytes, {}, ns, ns};
        if (st.stack.isEmpty()) {
            r.function = QString::fromLatin1(fallbackFn);
            m_agg.addRecv(r);
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
        for (LocationState &st : m_locations)
            while (!st.stack.isEmpty()) {
                const OpenRegion r = st.stack.takeLast();
                emitRegion(st, r, std::max(endNs, r.enterNs));
            }
    }

private:
    bool isMpi(OTF2_RegionRef region) const
    {
        const auto it = m_regions.constFind(region);
        return it != m_regions.cend() && it->mpi;
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
        m_agg.addCall({st.rank, info.kind, info.name, r.enterNs, leaveNs});
        for (P2PSend s : r.sends) {
            s.function = info.name;
            s.enterNs = r.enterNs;
            s.leaveNs = leaveNs;
            m_agg.addSend(s);
        }
        for (P2PRecv rv : r.recvs) {
            rv.function = info.name;
            rv.enterNs = r.enterNs;
            rv.leaveNs = leaveNs;
            m_agg.addRecv(rv);
        }
        // Collective records only for collective operations; handle creation etc. in
        // MPI_Init / MPI_Comm_* also carries MpiCollectiveEnd but is not communication.
        if (r.hasCollective && info.kind == CallKind::Collective) {
            Collective c = r.collective;
            c.function = info.name;
            c.enterNs = r.enterNs;
            c.leaveNs = leaveNs;
            m_agg.addCollective(c);
        }
    }

    TraceAggregator &m_agg;
    const std::atomic_bool *m_cancel;
    uint64_t m_resolution;
    uint64_t m_offset;
    QHash<OTF2_RegionRef, RegionInfo> m_regions;
    QHash<OTF2_CommRef, CommInfo> m_comms;
    QHash<OTF2_LocationRef, LocationState> m_locations;
    qint64 m_maxNs = 0;
};

// Common prologue of all event callbacks: cancellation check and location lookup.
#define EVT_PROLOGUE                                                   \
    auto *tr = static_cast<EventTranslator *>(ud);                     \
    if (tr->cancelled())                                               \
        return OTF2_CALLBACK_INTERRUPT;                                \
    LocationState *st = tr->location(loc);                             \
    if (!st)                                                           \
        return OTF2_CALLBACK_SUCCESS;                                  \
    const qint64 ns = tr->timeNs(time)

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
OTF2_CallbackCode onMpiIrecv(OTF2_LocationRef loc, OTF2_TimeStamp time, void *ud, OTF2_AttributeList *,
                             uint32_t sender, OTF2_CommRef comm, uint32_t tag, uint64_t bytes, uint64_t)
{
    EVT_PROLOGUE;
    tr->recv(*st, comm, sender, tag, bytes, ns, "MPI_Irecv");
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

// Selects the locations, reads their local definitions (mapping tables, clock offsets)
// and opens their event readers.
bool prepareLocations(OTF2_Reader *reader, const QList<OTF2_LocationRef> &locations, QString *error)
{
    for (OTF2_LocationRef loc : locations)
        OTF2_Reader_SelectLocation(reader, loc);

    const bool haveDefFiles = OTF2_Reader_OpenDefFiles(reader) == OTF2_SUCCESS;
    const OTF2_ErrorCode rc = OTF2_Reader_OpenEvtFiles(reader);
    if (rc != OTF2_SUCCESS) {
        *error = QStringLiteral("Cannot open OTF2 event files: %1").arg(Otf2ErrorCapture::message(rc));
        return false;
    }
    for (OTF2_LocationRef loc : locations) {
        if (haveDefFiles) {
            if (OTF2_DefReader *dr = OTF2_Reader_GetDefReader(reader, loc)) {
                uint64_t read = 0;
                OTF2_Reader_ReadAllLocalDefinitions(reader, dr, &read);
                OTF2_Reader_CloseDefReader(reader, dr);
            }
        }
        OTF2_Reader_GetEvtReader(reader, loc);
    }
    if (haveDefFiles)
        OTF2_Reader_CloseDefFiles(reader);
    return true;
}

OTF2_ErrorCode readEvents(OTF2_Reader *reader, EventTranslator &tr)
{
    OTF2_GlobalEvtReader *ger = OTF2_Reader_GetGlobalEvtReader(reader);
    if (!ger)
        return OTF2_ERROR_INVALID;
    OTF2_GlobalEvtReaderCallbacks *cb = OTF2_GlobalEvtReaderCallbacks_New();
    OTF2_GlobalEvtReaderCallbacks_SetEnterCallback(cb, &onEnter);
    OTF2_GlobalEvtReaderCallbacks_SetLeaveCallback(cb, &onLeave);
    OTF2_GlobalEvtReaderCallbacks_SetMpiSendCallback(cb, &onMpiSend);
    OTF2_GlobalEvtReaderCallbacks_SetMpiIsendCallback(cb, &onMpiIsend);
    OTF2_GlobalEvtReaderCallbacks_SetMpiRecvCallback(cb, &onMpiRecv);
    OTF2_GlobalEvtReaderCallbacks_SetMpiIrecvCallback(cb, &onMpiIrecv);
    OTF2_GlobalEvtReaderCallbacks_SetMpiCollectiveEndCallback(cb, &onMpiCollectiveEnd);
    OTF2_Reader_RegisterGlobalEvtCallbacks(reader, ger, cb, &tr);
    OTF2_GlobalEvtReaderCallbacks_Delete(cb);

    uint64_t read = 0;
    const OTF2_ErrorCode rc = OTF2_Reader_ReadAllGlobalEvents(reader, ger, &read);
    OTF2_Reader_CloseGlobalEvtReader(reader, ger);
    OTF2_Reader_CloseEvtFiles(reader);
    return rc;
}

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

} // namespace

bool readOtf2(const QString &anchorFile, TraceAggregator &agg, TraceMeta *meta, QString *error,
              const std::atomic_bool *cancel)
{
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
    OTF2_Reader_SetSerialCollectiveCallbacks(reader.get());

    GlobalDefs defs;
    if (!readGlobalDefs(reader.get(), defs, &err))
        return fail(error, err);

    // Ranks and their nodes, then the CPU-thread locations belonging to them.
    const QMap<OTF2_LocationGroupRef, int> ranks = rankPerLocationGroup(defs);
    if (ranks.isEmpty())
        return fail(error, QStringLiteral("No MPI processes found in %1").arg(anchorFile));
    for (auto it = ranks.cbegin(); it != ranks.cend(); ++it)
        agg.addRank(it.value(), nodeNameOf(defs, defs.locationGroups.value(it.key()).parent));

    EventTranslator tr(agg, defs, cancel);
    QList<OTF2_LocationRef> selected;
    for (auto it = defs.locations.cbegin(); it != defs.locations.cend(); ++it) {
        if (it->type != OTF2_LOCATION_TYPE_CPU_THREAD || it->events == 0 || !ranks.contains(it->group))
            continue;
        selected.append(it.key());
        tr.addLocation(it.key(), ranks.value(it->group));
    }

    if (!selected.isEmpty()) {
        if (!prepareLocations(reader.get(), selected, &err))
            return fail(error, err);
        const OTF2_ErrorCode rc = readEvents(reader.get(), tr);
        if (tr.cancelled())
            return fail(error, QStringLiteral("Cancelled"));
        if (rc != OTF2_SUCCESS)
            return fail(error, QStringLiteral("Cannot read OTF2 events: %1").arg(Otf2ErrorCapture::message(rc)));
    }

    const qint64 lengthNs = std::max(tr.durationNs(defs.length), tr.maxEventNs());
    tr.closeAll(lengthNs);

    if (meta) {
        meta->name = anchor.absoluteDir().dirName();
        meta->traceLengthNs = lengthNs;
        meta->realtimeStart = defs.realtime == OTF2_UNDEFINED_TIMESTAMP
            ? QDateTime()
            : QDateTime::fromMSecsSinceEpoch(qint64(defs.realtime / 1'000'000), QTimeZone::UTC);
    }
    return true;
}

} // namespace trace
