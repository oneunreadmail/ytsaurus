#include "discovery_server.h"
#include "discovery_server_service_proxy.h"
#include "group.h"
#include "group_manager.h"
#include "helpers.h"
#include "member.h"

#include <yt/ytlib/discovery_client/discovery_client_service_proxy.h>
#include <yt/ytlib/discovery_client/helpers.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/rpc/caching_channel_factory.h>
#include <yt/core/rpc/service_detail.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT::NDiscoveryServer {

using namespace NConcurrency;
using namespace NRpc;
using namespace NDiscoveryClient;
using namespace NYTree;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TClientDiscoveryService)

class TClientDiscoveryService
    : public TServiceBase
{
public:
    TClientDiscoveryService(
        IServerPtr rpcServer,
        TGroupManagerPtr groupManager,
        IInvokerPtr invoker)
        : TServiceBase(
            std::move(invoker),
            TDiscoveryClientServiceProxy::GetDescriptor(),
            DiscoveryServerLogger)
        , RpcServer_(std::move(rpcServer))
        , GroupManager_(std::move(groupManager))
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ListMembers));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetGroupMeta));

        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
    }

    void Initialize()
    {
        RpcServer_->RegisterService(this);
    }

    void Finalize()
    {
        RpcServer_->UnregisterService(this);
    }

private:
    const IServerPtr RpcServer_;
    const TGroupManagerPtr GroupManager_;

    DECLARE_RPC_SERVICE_METHOD(NDiscoveryClient::NProto, ListMembers)
    {
        const auto& groupId = request->group_id();
        auto options = FromProto<TListMembersOptions>(request->options());

        context->SetRequestInfo("GroupId: %v, Limit: %v",
            groupId,
            options.Limit);

        auto group = GroupManager_->GetGroupOrThrow(groupId);
        auto members = group->ListMembers(options.Limit);
        for (const auto& member : members) {
            auto* protoMember = response->add_members();
            protoMember->set_id(member->GetId());
            protoMember->set_priority(member->GetPriority());

            auto writer = member->CreateWriter();
            auto* memberAttributes = writer.GetAttributes();
            protoMember->mutable_attributes();
            for (const auto& key : options.AttributeKeys) {
                if (auto value = memberAttributes->FindYson(key)) {
                    auto* attr = protoMember->mutable_attributes()->add_attributes();
                    attr->set_key(key);
                    attr->set_value(value.ToString());
                }
            }
        }

        context->SetResponseInfo("MemberCount: %v", members.size());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NDiscoveryClient::NProto, GetGroupMeta)
    {
        const auto& groupId = request->group_id();

        context->SetRequestInfo("GroupId: %v",
            groupId);

        auto group = GroupManager_->GetGroupOrThrow(groupId);

        TGroupMeta meta;
        meta.MemberCount = group->GetMemberCount();
        ToProto(response->mutable_meta(), meta);

        context->SetResponseInfo("MemberCount: %v", meta.MemberCount);
        context->Reply();
    }


    DECLARE_RPC_SERVICE_METHOD(NDiscoveryClient::NProto, Heartbeat)
    {
        const auto& groupId = request->group_id();
        auto leaseTimeout = FromProto<TDuration>(request->lease_timeout());
        auto memberInfo = FromProto<TMemberInfo>(request->member_info());

        context->SetRequestInfo("GroupId: %v, MemberId: %v, LeaseTimeout: %v",
            groupId,
            memberInfo.Id,
            leaseTimeout);

        GroupManager_->ProcessHeartbeat(groupId, memberInfo, leaseTimeout);

        context->Reply();
    }
};

DEFINE_REFCOUNTED_TYPE(TClientDiscoveryService)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TServerDiscoveryService)

class TServerDiscoveryService
    : public TServiceBase
{
public:
    TServerDiscoveryService(
        IServerPtr rpcServer,
        TGroupManagerPtr groupManager,
        IInvokerPtr invoker,
        const TDiscoveryServerConfigPtr& config)
        : TServiceBase(
            std::move(invoker),
            TDiscoveryServerServiceProxy::GetDescriptor(),
            DiscoveryServerLogger)
        , RpcServer_(std::move(rpcServer))
        , GroupManager_(std::move(groupManager))
        , GossipBatchSize_(config->GossipBatchSize)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ProcessGossip));
    }

    void Initialize()
    {
        RpcServer_->RegisterService(this);
    }

    void Finalize()
    {
        RpcServer_->UnregisterService(this);
    }

private:
    const IServerPtr RpcServer_;
    const TGroupManagerPtr GroupManager_;
    const int GossipBatchSize_;

    DECLARE_RPC_SERVICE_METHOD(NProto, ProcessGossip)
    {
        context->SetRequestInfo("MemberCount: %v", request->members().size());

        std::vector<TGossipMemberInfo> membersBatch;
        for (const auto& protoMember : request->members()) {
            membersBatch.push_back(FromProto<TGossipMemberInfo>(protoMember));
            if (membersBatch.size() >= GossipBatchSize_) {
                GroupManager_->ProcessGossip(membersBatch);
                membersBatch.clear();
            }
        }
        if (!membersBatch.empty()) {
            GroupManager_->ProcessGossip(membersBatch);
        }

        context->Reply();
    }
};

DEFINE_REFCOUNTED_TYPE(TServerDiscoveryService)

////////////////////////////////////////////////////////////////////////////////

class TDiscoveryServer
    : public IDiscoveryServer
{
public:
    TDiscoveryServer(
        IServerPtr rpcServer,
        TString selfAddress,
        TDiscoveryServerConfigPtr config,
        IChannelFactoryPtr channelFactory,
        IInvokerPtr serverInvoker,
        IInvokerPtr gossipInvoker)
        : RpcServer_(std::move(rpcServer))
        , SelfAddress_(std::move(selfAddress))
        , Config_(std::move(config))
        , ChannelFactory_(CreateCachingChannelFactory(channelFactory))
        , Logger(DiscoveryServerLogger.WithTag("SelfAddress: %v", SelfAddress_))
        , GroupManager_(New<TGroupManager>(Logger))
        , GossipPeriodicExecutor_(New<TPeriodicExecutor>(
            std::move(gossipInvoker),
            BIND(&TDiscoveryServer::SendGossip, MakeWeak(this)),
            Config_->GossipPeriod))
        , ClientService_(New<TClientDiscoveryService>(
            RpcServer_,
            GroupManager_,
            serverInvoker))
        , ServerService_(New<TServerDiscoveryService>(
            RpcServer_,
            GroupManager_,
            serverInvoker,
            Config_))
    { }

    virtual void Initialize() override
    {
        ClientService_->Initialize();
        ServerService_->Initialize();

        GossipPeriodicExecutor_->Start();

        YT_LOG_INFO("Server initialized (Addresses: %v)", Config_->ServerAddresses);
    }

    virtual void Finalize() override
    {
        ClientService_->Finalize();
        ServerService_->Finalize();

        GossipPeriodicExecutor_->Stop();

        YT_LOG_INFO("Server finalized");
    }

    virtual NYTree::IYPathServicePtr GetYPathService() override
    {
        return GroupManager_->GetYPathService();
    }

private:
    const IServerPtr RpcServer_;
    const TString SelfAddress_;
    const TDiscoveryServerConfigPtr Config_;
    const IChannelFactoryPtr ChannelFactory_;
    const NLogging::TLogger Logger;
    const TGroupManagerPtr GroupManager_;
    const TPeriodicExecutorPtr GossipPeriodicExecutor_;
    const TClientDiscoveryServicePtr ClientService_;
    const TServerDiscoveryServicePtr ServerService_;

    void SendGossip()
    {
        auto modifiedMembers = GroupManager_->GetModifiedMembers();
        YT_LOG_DEBUG("Gossip started (ModifiedMemberCount: %v)", modifiedMembers.size());

        auto gossipStartTime = TInstant::Now();

        for (const auto& address : Config_->ServerAddresses) {
            if (address == SelfAddress_) {
                continue;
            }

            YT_LOG_DEBUG("Sending gossip (Address: %v)", address);

            auto channel = ChannelFactory_->CreateChannel(address);
            auto proxy = TDiscoveryServerServiceProxy(std::move(channel));
            auto req = proxy.ProcessGossip();

            for (const auto& member : modifiedMembers) {
                auto* protoMember = req->add_members();
                auto* memberInfo = protoMember->mutable_member_info();
                memberInfo->set_id(member->GetId());
                memberInfo->set_priority(member->GetPriority());

                {
                    auto reader = member->CreateReader();
                    if (gossipStartTime - member->GetLastGossipAttributesUpdateTime() > Config_->AttributesUpdatePeriod) {
                        YT_LOG_DEBUG("Sending attributes (Address: %v)", address);
                        ToProto(memberInfo->mutable_attributes(), *reader.GetAttributes());
                    }
                    memberInfo->set_revision(reader.GetRevision());
                }

                protoMember->set_group_id(member->GetGroupId());
                protoMember->set_lease_deadline(ToProto<i64>(member->GetLeaseDeadline()));
            }
            req->Invoke().Subscribe(
                BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TDiscoveryServerServiceProxy::TRspProcessGossipPtr>& rspOrError) {
                    if (rspOrError.IsOK()) {
                        YT_LOG_DEBUG("Gossip succeeded (Address: %v)", address);
                    } else {
                        YT_LOG_DEBUG(rspOrError, "Gossip failed (Address: %v)", address);
                    }
                }));
        }

        for (const auto& member : modifiedMembers) {
            if (gossipStartTime - member->GetLastGossipAttributesUpdateTime() > Config_->AttributesUpdatePeriod) {
                member->SetLastGossipAttributesUpdateTime(gossipStartTime);
            }
        }
    }
};

IDiscoveryServerPtr CreateDiscoveryServer(
    IServerPtr rpcServer,
    TString selfAddress,
    TDiscoveryServerConfigPtr config,
    IChannelFactoryPtr channelFactory,
    IInvokerPtr serverInvoker,
    IInvokerPtr gossipInvoker)
{
    return New<TDiscoveryServer>(
        std::move(rpcServer),
        std::move(selfAddress),
        std::move(config),
        std::move(channelFactory),
        std::move(serverInvoker),
        std::move(gossipInvoker));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryServer