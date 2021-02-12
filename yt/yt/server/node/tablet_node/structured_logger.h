#pragma once

#include "public.h"
#include "private.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/core/logging/log.h>
#include <yt/core/logging/fluent_log.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ELogEntryType,
    (FullHeartbeat)
    (IncrementalHeartbeat)
    (Event)
);

////////////////////////////////////////////////////////////////////////////////

/*!
 * Entry point for LSM event log.
 *
 * Serves two purposes:
 *  - serializes log messages and flushes them in order of receiving. Most messages
 *    arrive through child per-tablet loggers, though callers may invoke
 *    IStructuredLogger::LogEvent directly.
 *  - periodically triggers tablet heartbeat events, both full and incremental.
 *
 * Only one instance of this class should exist.
 */
struct IStructuredLogger
    : public TRefCounted
{
public:
    //! Log arbitrary event fluently.
    //! Thread affinity: any.
    virtual NLogging::TOneShotFluentLogEvent LogEvent(TStringBuf eventType) = 0;

    //! Create tablet-bound logger.
    //! Thread affinity: corresponding automaton.
    virtual IPerTabletStructuredLoggerPtr CreateLogger(TTablet* tablet) = 0;

    //! Trigger all tablet heartbeats. If |initial|, splay is added
    //! to Last[Full|Incremental]HeartbeatTime.
    //! Thread affinity: corresponding automaton.
    virtual void OnHeartbeatRequest(const TTabletManagerPtr& tabletManager, bool initial) = 0;
};

DEFINE_REFCOUNTED_TYPE(IStructuredLogger)

////////////////////////////////////////////////////////////////////////////////

/*!
 * Entry point for LSM event log bound to a tablet.
 *
 * All messages are serialized by owning IStructuredLogger instance. All messages
 * are supplemented with tablet_id tag.
 *
 * Thread affinity: corresponding automaton, unless stated otherwise.
 *
 * Some events are logged fluently by the caller using LogEvent(), others
 * are implemented in this class (OnSmthHappened methods). There is no actual
 * difference, dedicated methods are mostly used to incapsulate clumsy
 * or repeating lines of logging.
 *
 * NB: though unlikely, the logger may outlive its tablet. All methods except
 * GetTabletId() and LogEvent() should be called only if lifetime of the tablet
 * is guaranteed by the caller.
 */
struct IPerTabletStructuredLogger
    : public TRefCounted
{
    //! Thread affinity: any.
    virtual TTabletId GetTabletId() const = 0;

    //! Toggle actual logging.
    //! Thread affinity: any.
    virtual void SetEnabled(bool enabled) = 0;

    //! Log arbitrary event fluently.
    //! Thread affinity: any.
    virtual NLogging::TOneShotFluentLogEvent LogEvent(TStringBuf eventType) = 0;

    //! Log all tablet meta.
    virtual void OnFullHeartbeat() = 0;

    //! Log fluent tablet meta like dynamic store sizes.
    virtual void OnIncrementalHeartbeat() = 0;

    virtual void OnStoreRotated(
        const IDynamicStorePtr& previousStore,
        const IDynamicStorePtr& newStore) = 0;
    virtual void OnBackingStoreSet(
        const IChunkStorePtr& store,
        const IDynamicStorePtr& backingStore) = 0;
    virtual void OnBackingStoreReleased(const IChunkStorePtr& store) = 0;

    virtual void OnTabletStoresUpdatePrepared(
        const TStoreIdList& addedStoreIds,
        const TStoreIdList& removedStoreIds,
        NTabletClient::ETabletStoresUpdateReason updateReason,
        TTransactionId transactionId) = 0;

    virtual void OnTabletStoresUpdateCommitted(
        const std::vector<IStorePtr>& addedStores,
        const std::vector<TStoreId>& removedStoreIds,
        NTabletClient::ETabletStoresUpdateReason updateReason,
        TDynamicStoreId allocatedDynamicStoreId,
        TTransactionId transactionId) = 0;

    virtual void OnPartitionStateChanged(const TPartition* partition) = 0;
    virtual void OnStoreStateChanged(const IStorePtr& store) = 0;
    virtual void OnStoreCompactionStateChanged(const IChunkStorePtr& store) = 0;
    virtual void OnStorePreloadStateChanged(const IChunkStorePtr& store) = 0;
    virtual void OnStoreFlushStateChanged(const IDynamicStorePtr& store) = 0;

    virtual void OnPartitionSplit(
        const TPartition* oldPartition,
        int partitionIndex,
        int splitFactor) = 0;

    virtual void OnPartitionsMerged(
        const std::vector<TPartitionId>& oldPartitionIds,
        const TPartition* newPartition) = 0;
};

DEFINE_REFCOUNTED_TYPE(IPerTabletStructuredLogger)

////////////////////////////////////////////////////////////////////////////////

IStructuredLoggerPtr CreateStructuredLogger(const NClusterNode::TBootstrap* bootstrap);

IPerTabletStructuredLoggerPtr CreateMockPerTabletStructuredLogger(TTablet* tablet);

} // namespace NYT::NTabletNode