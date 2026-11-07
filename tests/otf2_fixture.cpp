#include "otf2_fixture.h"

#include <QDateTime>
#include <QDir>
#include <QTimeZone>

#include <otf2/otf2.h>

#include <array>

namespace {

constexpr uint64_t kResolution = 1'000'000'000; // ticks per second
constexpr uint64_t kOffset = 5'000'000'000;     // global offset: 5 s
constexpr uint64_t kTicksPerMs = 1'000'000;
constexpr uint64_t kLengthMs = 2500;
constexpr int kRanks = 4;

// Definition ids
enum StringId : OTF2_StringRef {
    S_Empty, S_Machine, S_MachineClass, S_NodeA, S_NodeB, S_NodeClass,
    S_Rank0, S_Rank1, S_Rank2, S_Rank3, S_Thread,
    S_Main, S_Send, S_Recv, S_Allreduce, S_Ssend,
    S_CommLocations, S_World, S_Sub,
};
enum RegionId : OTF2_RegionRef { R_Main, R_Send, R_Recv, R_Allreduce, R_Ssend };
enum GroupId : OTF2_GroupRef { G_Locations, G_World, G_Sub };
enum CommId : OTF2_CommRef { C_World, C_Sub };
enum NodeId : OTF2_SystemTreeNodeRef { N_Machine, N_NodeA, N_NodeB };

OTF2_FlushType preFlush(void *, OTF2_FileType, OTF2_LocationRef, void *, bool)
{
    return OTF2_FLUSH;
}

OTF2_TimeStamp postFlush(void *, OTF2_FileType, OTF2_LocationRef)
{
    return kOffset + kLengthMs * kTicksPerMs;
}

OTF2_FlushCallbacks gFlushCallbacks = {preFlush, postFlush};

// Writes one rank's events and counts them for the Location definition.
class RankWriter
{
public:
    RankWriter(OTF2_Archive *archive, int rank)
        : m_writer(OTF2_Archive_GetEvtWriter(archive, OTF2_LocationRef(rank)))
    {
    }
    OTF2_EvtWriter *handle() const { return m_writer; }
    uint64_t events() const { return m_events; }
    bool ok() const { return m_writer && m_ok; }

    void enter(uint64_t ms, OTF2_RegionRef r) { check(OTF2_EvtWriter_Enter(m_writer, nullptr, t(ms), r)); }
    void leave(uint64_t ms, OTF2_RegionRef r) { check(OTF2_EvtWriter_Leave(m_writer, nullptr, t(ms), r)); }

    // MPI_Send / MPI_Ssend region [enterMs, leaveMs] with the MpiSend event at enterMs.
    void send(OTF2_RegionRef r, uint64_t enterMs, uint64_t leaveMs, uint32_t to, OTF2_CommRef comm,
              uint32_t tag, uint64_t bytes)
    {
        enter(enterMs, r);
        check(OTF2_EvtWriter_MpiSend(m_writer, nullptr, t(enterMs), to, comm, tag, bytes));
        leave(leaveMs, r);
    }

    // MPI_Recv region [enterMs, leaveMs] with the MpiRecv event at leaveMs; leave optional.
    void recv(uint64_t enterMs, uint64_t leaveMs, uint32_t from, OTF2_CommRef comm, uint32_t tag,
              uint64_t bytes, bool withLeave = true)
    {
        enter(enterMs, R_Recv);
        check(OTF2_EvtWriter_MpiRecv(m_writer, nullptr, t(leaveMs), from, comm, tag, bytes));
        if (withLeave)
            leave(leaveMs, R_Recv);
    }

    void allreduce(uint64_t enterMs, uint64_t leaveMs)
    {
        enter(enterMs, R_Allreduce);
        check(OTF2_EvtWriter_MpiCollectiveBegin(m_writer, nullptr, t(enterMs)));
        check(OTF2_EvtWriter_MpiCollectiveEnd(m_writer, nullptr, t(leaveMs), OTF2_COLLECTIVE_OP_ALLREDUCE,
                                              C_World, OTF2_COLLECTIVE_ROOT_NONE, 64, 64));
        leave(leaveMs, R_Allreduce);
    }

private:
    static OTF2_TimeStamp t(uint64_t ms) { return kOffset + ms * kTicksPerMs; }
    void check(OTF2_ErrorCode rc)
    {
        ++m_events;
        m_ok = m_ok && rc == OTF2_SUCCESS;
    }

    OTF2_EvtWriter *m_writer;
    uint64_t m_events = 0;
    bool m_ok = true;
};

bool writeEvents(OTF2_Archive *archive, bool truncated, std::array<uint64_t, kRanks> &counts)
{
    bool ok = true;
    for (int rank = 0; rank < kRanks; ++rank) {
        RankWriter w(archive, rank);
        const bool cutR0 = truncated && rank == 0;
        w.enter(0, R_Main);
        switch (rank) {
        case 0:
            w.send(R_Send, 100, 101, 1, C_World, 7, 1000);
            w.recv(101, 402, 3, C_World, 7, 1000, !cutR0);
            w.allreduce(1000, 1201);
            break;
        case 1:
            w.send(R_Send, 100, 101, 2, C_World, 7, 1000);
            w.recv(101, 102, 0, C_World, 7, 1000);
            w.allreduce(1000, 1201);
            w.send(R_Ssend, 2000, 2301, 2, C_World, 9, 500);
            break;
        case 2:
            w.send(R_Send, 100, 101, 3, C_World, 7, 1000);
            w.recv(101, 102, 1, C_World, 7, 1000);
            w.allreduce(1200, 1201);
            w.recv(2100, 2101, 1, C_Sub, 5, 200); // SUB-local 1 = world 3
            w.recv(2300, 2301, 1, C_World, 9, 500);
            break;
        case 3:
            w.recv(101, 102, 2, C_World, 7, 1000);
            w.send(R_Send, 400, 401, 0, C_World, 7, 1000);
            w.allreduce(1000, 1201);
            w.send(R_Send, 2050, 2051, 0, C_Sub, 5, 200); // SUB-local 0 = world 2
            w.send(R_Send, 2400, 2401, 0, C_World, 11, 100); // never received
            break;
        }
        if (!cutR0)
            w.leave(kLengthMs, R_Main);
        ok = ok && w.ok();
        counts[rank] = w.events();
        ok = ok && OTF2_Archive_CloseEvtWriter(archive, w.handle()) == OTF2_SUCCESS;
    }
    return ok;
}

bool writeLocalDefs(OTF2_Archive *archive)
{
    if (OTF2_Archive_OpenDefFiles(archive) != OTF2_SUCCESS)
        return false;
    bool ok = true;
    for (int rank = 0; rank < kRanks; ++rank) {
        OTF2_DefWriter *dw = OTF2_Archive_GetDefWriter(archive, OTF2_LocationRef(rank));
        ok = ok && dw && OTF2_Archive_CloseDefWriter(archive, dw) == OTF2_SUCCESS;
    }
    return OTF2_Archive_CloseDefFiles(archive) == OTF2_SUCCESS && ok;
}

bool writeGlobalDefs(OTF2_Archive *archive, bool withRealtime, const std::array<uint64_t, kRanks> &counts)
{
    OTF2_GlobalDefWriter *g = OTF2_Archive_GetGlobalDefWriter(archive);
    if (!g)
        return false;
    bool ok = true;
    auto check = [&ok](OTF2_ErrorCode rc) { ok = ok && rc == OTF2_SUCCESS; };

    const uint64_t realtime = withRealtime
        ? uint64_t(QDateTime(QDate(2026, 1, 1), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch()) * 1'000'000
        : OTF2_UNDEFINED_TIMESTAMP;
    check(OTF2_GlobalDefWriter_WriteClockProperties(g, kResolution, kOffset, kLengthMs * kTicksPerMs, realtime));

    const char *strings[] = {
        "", "machine", "machine", "nodeA", "nodeB", "node",
        "Rank 0", "Rank 1", "Rank 2", "Rank 3", "Master thread",
        "main", "MPI_Send", "MPI_Recv", "MPI_Allreduce", "MPI_Ssend",
        "", "MPI_COMM_WORLD", "SUB",
    };
    for (OTF2_StringRef i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i)
        check(OTF2_GlobalDefWriter_WriteString(g, i, strings[i]));

    check(OTF2_GlobalDefWriter_WriteSystemTreeNode(g, N_Machine, S_Machine, S_MachineClass,
                                                   OTF2_UNDEFINED_SYSTEM_TREE_NODE));
    check(OTF2_GlobalDefWriter_WriteSystemTreeNode(g, N_NodeA, S_NodeA, S_NodeClass, N_Machine));
    check(OTF2_GlobalDefWriter_WriteSystemTreeNode(g, N_NodeB, S_NodeB, S_NodeClass, N_Machine));

    for (int rank = 0; rank < kRanks; ++rank) {
        check(OTF2_GlobalDefWriter_WriteLocationGroup(g, OTF2_LocationGroupRef(rank), S_Rank0 + rank,
                                                      OTF2_LOCATION_GROUP_TYPE_PROCESS,
                                                      rank < 2 ? N_NodeA : N_NodeB,
                                                      OTF2_UNDEFINED_LOCATION_GROUP));
        check(OTF2_GlobalDefWriter_WriteLocation(g, OTF2_LocationRef(rank), S_Thread,
                                                 OTF2_LOCATION_TYPE_CPU_THREAD, counts[rank],
                                                 OTF2_LocationGroupRef(rank)));
    }

    auto region = [&](OTF2_RegionRef id, OTF2_StringRef name, OTF2_RegionRole role, OTF2_Paradigm paradigm) {
        check(OTF2_GlobalDefWriter_WriteRegion(g, id, name, name, S_Empty, role, paradigm,
                                               OTF2_REGION_FLAG_NONE, S_Empty, 0, 0));
    };
    region(R_Main, S_Main, OTF2_REGION_ROLE_FUNCTION, OTF2_PARADIGM_USER);
    region(R_Send, S_Send, OTF2_REGION_ROLE_POINT2POINT, OTF2_PARADIGM_MPI);
    region(R_Recv, S_Recv, OTF2_REGION_ROLE_POINT2POINT, OTF2_PARADIGM_MPI);
    region(R_Allreduce, S_Allreduce, OTF2_REGION_ROLE_COLL_ALL2ALL, OTF2_PARADIGM_MPI);
    region(R_Ssend, S_Ssend, OTF2_REGION_ROLE_POINT2POINT, OTF2_PARADIGM_MPI);

    const uint64_t locations[] = {0, 1, 2, 3};
    const uint64_t world[] = {0, 1, 2, 3};
    const uint64_t sub[] = {2, 3};
    check(OTF2_GlobalDefWriter_WriteGroup(g, G_Locations, S_CommLocations, OTF2_GROUP_TYPE_COMM_LOCATIONS,
                                          OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, 4, locations));
    check(OTF2_GlobalDefWriter_WriteGroup(g, G_World, S_Empty, OTF2_GROUP_TYPE_COMM_GROUP,
                                          OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, 4, world));
    check(OTF2_GlobalDefWriter_WriteGroup(g, G_Sub, S_Empty, OTF2_GROUP_TYPE_COMM_GROUP,
                                          OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, 2, sub));
    check(OTF2_GlobalDefWriter_WriteComm(g, C_World, S_World, G_World, OTF2_UNDEFINED_COMM,
                                         OTF2_COMM_FLAG_NONE));
    check(OTF2_GlobalDefWriter_WriteComm(g, C_Sub, S_Sub, G_Sub, C_World, OTF2_COMM_FLAG_NONE));
    return ok;
}

QString writeArchive(const QString &dir, bool withRealtime, bool truncated)
{
    if (!QDir().mkpath(dir))
        return {};
    const QByteArray path = QDir(dir).absolutePath().toUtf8();
    OTF2_Archive *archive = OTF2_Archive_Open(path.constData(), "traces", OTF2_FILEMODE_WRITE,
                                              1024 * 1024, 4 * 1024 * 1024, OTF2_SUBSTRATE_POSIX,
                                              OTF2_COMPRESSION_NONE);
    if (!archive)
        return {};

    bool ok = OTF2_Archive_SetFlushCallbacks(archive, &gFlushCallbacks, nullptr) == OTF2_SUCCESS
           && OTF2_Archive_SetSerialCollectiveCallbacks(archive) == OTF2_SUCCESS
           && OTF2_Archive_OpenEvtFiles(archive) == OTF2_SUCCESS;

    std::array<uint64_t, kRanks> counts{};
    ok = ok && writeEvents(archive, truncated, counts);
    ok = ok && OTF2_Archive_CloseEvtFiles(archive) == OTF2_SUCCESS;
    ok = ok && writeLocalDefs(archive);
    ok = ok && writeGlobalDefs(archive, withRealtime, counts);
    ok = (OTF2_Archive_Close(archive) == OTF2_SUCCESS) && ok;
    return ok ? QDir(dir).filePath(QStringLiteral("traces.otf2")) : QString();
}

} // namespace

QString writeRing4Otf2(const QString &dir, bool withRealtime)
{
    return writeArchive(dir, withRealtime, false);
}

QString writeTruncatedOtf2(const QString &dir)
{
    return writeArchive(dir, true, true);
}
