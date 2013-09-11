/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/service_chaining.h"
#include "bgp/routing-instance/static_route.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/routing-instance/routepath_replicator.h"

#include <fstream>
#include <algorithm>

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/assign/list_of.hpp>

#include "base/test/task_test_util.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_session_manager.h"
#include "bgp/bgp_xmpp_channel.h"
#include "bgp/community.h"
#include "bgp/inet/inet_table.h"
#include "bgp/l3vpn/inetvpn_route.h"
#include "bgp/l3vpn/inetvpn_table.h"
#include "bgp/origin-vn/origin_vn.h"
#include "bgp/security_group/security_group.h"
#include "bgp/tunnel_encap/tunnel_encap.h"
#include "bgp/test/bgp_server_test_util.h"
#include "bgp/test/bgp_test_util.h"
#include "control-node/control_node.h"
#include "control-node/test/network_agent_mock.h"
#include "db/db_graph.h"
#include "db/test/db_test_util.h"
#include "ifmap/ifmap_link_table.h"
#include "ifmap/ifmap_server_parser.h"
#include "ifmap/ifmap_table.h"
#include "ifmap/test/ifmap_test_util.h"
#include "io/event_manager.h"
#include "io/test/event_manager_test.h"
#include <pugixml/pugixml.hpp>
#include "schema/bgp_schema_types.h"
#include "schema/vnc_cfg_types.h"
#include "testing/gunit.h"
#include "xmpp/xmpp_client.h"
#include "xmpp/xmpp_init.h"
#include "xmpp/xmpp_server.h"

using namespace std;
using boost::assign::list_of;
using boost::assign::map_list_of;
using boost::system::error_code;
using namespace pugi;

#define VERIFY_EQ(expected, actual) \
    TASK_UTIL_EXPECT_EQ(expected, actual)

class BgpXmppChannelMock : public BgpXmppChannel {
public:
    BgpXmppChannelMock(XmppChannel *channel, BgpServer *server, 
            BgpXmppChannelManager *manager) : 
        BgpXmppChannel(channel, server, manager), count_(0) {
            bgp_policy_ = RibExportPolicy(BgpProto::XMPP,
                                          RibExportPolicy::XMPP, -1, 0);
    }

    virtual void ReceiveUpdate(const XmppStanza::XmppMessage *msg) {
        count_ ++;
        BgpXmppChannel::ReceiveUpdate(msg);
    }

    size_t Count() const { return count_; }
    void ResetCount() { count_ = 0; }
    virtual ~BgpXmppChannelMock() { }

private:
    size_t count_;
};

class BgpXmppChannelManagerMock : public BgpXmppChannelManager {
public:
    BgpXmppChannelManagerMock(XmppServer *x, BgpServer *b) :
        BgpXmppChannelManager(x, b), count(0), channels(0) { }

    virtual void XmppHandleChannelEvent(XmppChannel *channel,
                                        xmps::PeerState state) {
         count++;
         BgpXmppChannelManager::XmppHandleChannelEvent(channel, state);
    }

    virtual BgpXmppChannel *CreateChannel(XmppChannel *channel) {
        channel_[channels] = new BgpXmppChannelMock(channel, bgp_server_, this);
        channels++;
        return channel_[channels-1];
    }

    int Count() {
        return count;
    }
    int count;
    int channels;
    BgpXmppChannelMock *channel_[2];
};


static const char *config_control_node= "\
<config>\
    <routing-instance name='default-domain:default-project:ip-fabric:__default__'>\
    <bgp-router name=\'CN1\'>\
        <identifier>192.168.0.1</identifier>\
        <address>127.0.0.1</address>\
        <port>%d</port>\
        <address-families>\
            <family>inet-vpn</family>\
        </address-families>\
        <session to=\'CN2\'>\
            <address-families>\
                <family>inet-vpn</family>\
            </address-families>\
        </session>\
        <session to=\'MX\'>\
            <address-families>\
                <family>inet-vpn</family>\
            </address-families>\
        </session>\
    </bgp-router>\
    <bgp-router name=\'CN2\'>\
        <identifier>192.168.0.2</identifier>\
        <address>127.0.0.1</address>\
        <port>%d</port>\
        <address-families>\
            <family>inet-vpn</family>\
        </address-families>\
        <session to=\'MX\'>\
            <address-families>\
                <family>inet-vpn</family>\
            </address-families>\
        </session>\
    </bgp-router>\
    <bgp-router name=\'MX\'>\
        <identifier>192.168.0.3</identifier>\
        <address>127.0.0.1</address>\
        <port>%d</port>\
        <address-families>\
            <family>inet-vpn</family>\
        </address-families>\
    </bgp-router>\
    </routing-instance>\
</config>\
";

static const char *config_delete = "\
<delete>\
    <bgp-router name=\'CN1\'>\
    </bgp-router>\
    <bgp-router name=\'CN2\'>\
    </bgp-router>\
    <bgp-router name=\'MX\'>\
    </bgp-router>\
</delete>\
";
static const char *config_mx_vrf = "\
<config>\
    <routing-instance name='blue'>\
        <vrf-target>target:64496:1</vrf-target>\
        <vrf-target>target:1:4</vrf-target>\
    </routing-instance>\
    <routing-instance name='public'>\
        <vrf-target>target:1:1</vrf-target>\
        <vrf-target>\
            target:64496:4\
            <import-export>export</import-export>\
        </vrf-target>\
    </routing-instance>\
</config>\
";

class ServiceChainTest : public ::testing::Test {
protected:
    ServiceChainTest() : thread_(&evm_) { }

    virtual void SetUp() {
        IFMapServerParser *parser = IFMapServerParser::GetInstance("schema");
        bgp_schema_ParserInit(parser);
        vnc_cfg_ParserInit(parser);

        cn1_.reset(new BgpServerTest(&evm_, "CN1"));
        cn1_->session_manager()->Initialize(0);
        LOG(DEBUG, "Created Control-Node 1 at port: " <<
            cn1_->session_manager()->GetPort());

        cn1_xmpp_server_ = new XmppServer(&evm_, test::XmppDocumentMock::kControlNodeJID);
        cn1_xmpp_server_->Initialize(0, false);
        LOG(DEBUG, "Created XMPP server at port: " <<
            cn1_xmpp_server_->GetPort());

        cn2_.reset(new BgpServerTest(&evm_, "CN2"));
        cn2_->session_manager()->Initialize(0);
        LOG(DEBUG, "Created Control-Node 2 at port: " <<
            cn2_->session_manager()->GetPort());

        cn2_xmpp_server_ = new XmppServer(&evm_, test::XmppDocumentMock::kControlNodeJID);
        cn2_xmpp_server_->Initialize(0, false);
        LOG(DEBUG, "Created XMPP server at port: " <<
            cn2_xmpp_server_->GetPort());
        mx_.reset(new BgpServerTest(&evm_, "MX"));
        mx_->session_manager()->Initialize(0);
        LOG(DEBUG, "Created MX at port: " << mx_->session_manager()->GetPort());

        if (aggregate_enable_) {
            cn1_->service_chain_mgr()->set_aggregate_host_route(true);
            cn2_->service_chain_mgr()->set_aggregate_host_route(true);
        }

        bgp_channel_manager_cn1_.reset(
            new BgpXmppChannelManagerMock(cn1_xmpp_server_, cn1_.get()));

        bgp_channel_manager_cn2_.reset(
            new BgpXmppChannelManagerMock(cn2_xmpp_server_, cn2_.get()));

        task_util::WaitForIdle();

        thread_.Start();
        Configure();
        task_util::WaitForIdle();
        // Create XMPP Agent on compute node 1 connected to XMPP server 
        // Control-node-1
        agent_a_1_.reset(new test::NetworkAgentMock(&evm_, "agent-a", 
                                                    cn1_xmpp_server_->GetPort(),
                                                    "127.0.0.1"));

        // Create XMPP Agent on compute node 1 connected to XMPP server 
        // Control-node-2
        agent_a_2_.reset(new test::NetworkAgentMock(&evm_, "agent-a", 
                                                    cn2_xmpp_server_->GetPort(),
                                                    "127.0.0.1"));

        // Create XMPP Agent on compute node 2 connected to XMPP server 
        // Control-node-1 
        agent_b_1_.reset(new test::NetworkAgentMock(&evm_, "agent-b", 
                                                    cn1_xmpp_server_->GetPort(),
                                                    "127.0.0.2"));


        // Create XMPP Agent on compute node 2 connected to XMPP server Control-node-2 
        agent_b_2_.reset(new test::NetworkAgentMock(&evm_, "agent-b", 
                                                    cn2_xmpp_server_->GetPort(),
                                                    "127.0.0.2"));

        TASK_UTIL_EXPECT_TRUE(agent_a_1_->IsEstablished());
        TASK_UTIL_EXPECT_TRUE(agent_b_1_->IsEstablished());
        TASK_UTIL_EXPECT_TRUE(agent_a_2_->IsEstablished());
        TASK_UTIL_EXPECT_TRUE(agent_b_2_->IsEstablished());
        TASK_UTIL_EXPECT_EQ(2, bgp_channel_manager_cn1_->NumUpPeer());
        TASK_UTIL_EXPECT_EQ(2, bgp_channel_manager_cn2_->NumUpPeer());

        agent_a_1_->Subscribe("blue-i1", 1); 
        agent_a_2_->Subscribe("blue-i1", 1); 
        agent_b_1_->Subscribe("blue-i1", 1); 
        agent_b_2_->Subscribe("blue-i1", 1); 

        agent_a_1_->Subscribe("blue", 2); 
        agent_a_2_->Subscribe("blue", 2); 
        agent_b_1_->Subscribe("blue", 2); 
        agent_b_2_->Subscribe("blue", 2); 

        agent_a_1_->Subscribe("red", 3); 
        agent_a_2_->Subscribe("red", 3); 
        agent_b_1_->Subscribe("red", 3); 
        agent_b_2_->Subscribe("red", 3); 
        task_util::WaitForIdle();
    }

    virtual void TearDown() {
        // Close the sessions.
        agent_a_1_->SessionDown();
        agent_a_2_->SessionDown();
        agent_b_1_->SessionDown();
        agent_b_2_->SessionDown();

        Unconfigure();

        task_util::WaitForIdle();


        cn1_->Shutdown();
        task_util::WaitForIdle();
        cn1_xmpp_server_->Shutdown();
        task_util::WaitForIdle();

        cn2_->Shutdown();
        task_util::WaitForIdle();
        cn2_xmpp_server_->Shutdown();
        task_util::WaitForIdle();

        mx_->Shutdown();
        task_util::WaitForIdle();

        bgp_channel_manager_cn1_.reset();

        bgp_channel_manager_cn2_.reset();

        TcpServerManager::DeleteServer(cn1_xmpp_server_);
        cn1_xmpp_server_ = NULL;
        TcpServerManager::DeleteServer(cn2_xmpp_server_);
        cn2_xmpp_server_ = NULL;

        if (agent_a_1_) { agent_a_1_->Delete(); }
        if (agent_b_1_) { agent_b_1_->Delete(); }
        if (agent_a_2_) { agent_a_2_->Delete(); }
        if (agent_b_2_) { agent_b_2_->Delete(); }

        IFMapCleanUp();
        task_util::WaitForIdle();

        evm_.Shutdown();
        thread_.Join();
        task_util::WaitForIdle();
    }


    void IFMapCleanUp() {
        IFMapServerParser::GetInstance("vnc_cfg")->MetadataClear("vnc_cfg");
        IFMapServerParser::GetInstance("schema")->MetadataClear("schema");
    }


    void VerifyAllPeerUp(BgpServerTest *server) {
        TASK_UTIL_EXPECT_EQ_MSG(2, server->num_bgp_peer(), "Wait for all peers to get configured");
        TASK_UTIL_EXPECT_EQ_MSG(2, server->NumUpPeer(), "Wait for all peers to come up");

        LOG(DEBUG, "All Peers are up: " << server->localname());
    }

    void Configure() {
        char config[4096];
        snprintf(config, sizeof(config), config_control_node,
                 cn1_->session_manager()->GetPort(),
                 cn2_->session_manager()->GetPort(),
                 mx_->session_manager()->GetPort());
        cn1_->Configure(config);
        task_util::WaitForIdle();
        cn2_->Configure(config);
        task_util::WaitForIdle();
        mx_->Configure(config);
        task_util::WaitForIdle();

        VerifyAllPeerUp(cn1_.get());
        VerifyAllPeerUp(cn2_.get());
        VerifyAllPeerUp(mx_.get());

        vector<string> instance_names = list_of("blue")("blue-i1")("red-i2")("red");
        multimap<string, string> connections = 
            map_list_of("blue", "blue-i1") ("red-i2", "red");
        NetworkConfig(instance_names, connections);

        VerifyNetworkConfig(cn1_.get(), instance_names);
        VerifyNetworkConfig(cn2_.get(), instance_names);

        std::auto_ptr<autogen::ServiceChainInfo> params = 
            GetChainConfig("src/bgp/testdata/service_chain_1.xml");
        SetServiceChainProperty(cn1_.get(), params);
        task_util::WaitForIdle();

        params = GetChainConfig("src/bgp/testdata/service_chain_1.xml");
        SetServiceChainProperty(cn2_.get(), params);
        task_util::WaitForIdle();

        mx_->Configure(config_mx_vrf);
    }

    void NetworkConfig(const vector<string> &instance_names,
                       const multimap<string, string> &connections) {
        string netconf(
            bgp_util::NetworkConfigGenerate(instance_names, connections));
        IFMapServerParser *parser = IFMapServerParser::GetInstance("schema");
        parser->Receive(cn1_->config_db(), netconf.data(), netconf.length(), 0);
        task_util::WaitForIdle();
        parser->Receive(cn2_->config_db(), netconf.data(), netconf.length(), 0);
        task_util::WaitForIdle();
    }

    void VerifyNetworkConfig(BgpServerTest *server, const vector<string> &instance_names) {
        for (vector<string>::const_iterator iter = instance_names.begin();
             iter != instance_names.end(); ++iter) {
            TASK_UTIL_WAIT_NE_NO_MSG(server->routing_instance_mgr()->GetRoutingInstance(*iter),
                NULL, 1000, 10000, "Wait for routing instance..");
            const RoutingInstance *rti = server->routing_instance_mgr()->GetRoutingInstance(*iter);
            TASK_UTIL_WAIT_NE_NO_MSG(rti->virtual_network_index(),
                0, 1000, 10000, "Wait for vn index..");
        }
    }

    void Unconfigure() {

        RemoveServiceChainProperty(cn1_.get());
        RemoveServiceChainProperty(cn2_.get());

        char config[4096];
        snprintf(config, sizeof(config), config_delete);
        cn1_->Configure(config);
        cn2_->Configure(config);
        mx_->Configure(config);
    }

    void DisableServiceChainQ(BgpServerTest *server) {
        server->service_chain_mgr()->DisableQueue();
    }

    void EnableServiceChainQ(BgpServerTest *server) {
        server->service_chain_mgr()->EnableQueue();
    }

    int RouteCount(BgpServerTest *server, const string &instance_name) const {
        string tablename(instance_name);
        tablename.append(".inet.0");
        BgpTable *table = static_cast<BgpTable *>(
            server->database()->FindTable(tablename));
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return 0;
        }
        return table->Size();
    }

    BgpRoute *InetRouteLookup(BgpServerTest *server, const string &instance_name,
                              const string &prefix) {
        BgpTable *table = static_cast<BgpTable *>(
            server->database()->FindTable(instance_name + ".inet.0"));
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return NULL;
        }
        error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        InetTable::RequestKey key(nlri, NULL);
        BgpRoute *rt = static_cast<BgpRoute *>(table->Find(&key));
        return rt;
    }

    void AddInetRoute(BgpServerTest *server, IPeer *peer, const string &instance_name,
                      const string &prefix, int localpref, 
                      std::vector<uint32_t> sglist = std::vector<uint32_t>(),
                      std::set<string> encap = std::set<string>(),
                      string nexthop="7.8.9.1", 
                      uint32_t flags=0, int label=303) {
        error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        request.key.reset(new InetTable::RequestKey(nlri, peer));

        BgpAttrSpec attr_spec;
        boost::scoped_ptr<BgpAttrLocalPref> local_pref(
                                                       new BgpAttrLocalPref(localpref));
        attr_spec.push_back(local_pref.get());

        IpAddress chain_addr = Ip4Address::from_string(nexthop, error);
        boost::scoped_ptr<BgpAttrNextHop> 
            nexthop_attr(new BgpAttrNextHop(chain_addr.to_v4().to_ulong()));
        attr_spec.push_back(nexthop_attr.get());

        ExtCommunitySpec ext_comm;
        for(std::vector<uint32_t>::iterator it = sglist.begin(); 
            it != sglist.end(); it++) {
            SecurityGroup sgid(0, *it);
            ext_comm.communities.push_back(sgid.GetExtCommunityValue());
        }
        for(std::set<string>::iterator it = encap.begin(); 
            it != encap.end(); it++) {
            TunnelEncap tunnel_encap(*it);
            ext_comm.communities.push_back(tunnel_encap.GetExtCommunityValue());
        }
        attr_spec.push_back(&ext_comm);

        BgpAttrPtr attr = server->attr_db()->Locate(attr_spec);

        request.data.reset(new BgpTable::RequestData(attr, flags, label));
        BgpTable *table = static_cast<BgpTable *>(server->database()->FindTable(instance_name + ".inet.0"));
        ASSERT_TRUE(table != NULL);
        table->Enqueue(&request);
    }

    void DeleteInetRoute(BgpServerTest *server, IPeer *peer, const string &instance_name,
                         const string &prefix) {
        error_code error;
        Ip4Prefix nlri = Ip4Prefix::FromString(prefix, &error);
        EXPECT_FALSE(error);

        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_DELETE;
        request.key.reset(new InetTable::RequestKey(nlri, peer));

        BgpTable *table = static_cast<BgpTable *>(server->database()->FindTable(instance_name + ".inet.0"));
        ASSERT_TRUE(table != NULL);

        table->Enqueue(&request);
    }

    void AddConnectedRoute() {
        // Add Connected route
        if (mx_push_connected_) {
            AddInetRoute(mx_.get(), NULL, "blue", "1.1.2.3/32", 100);
        } else {
            agent_a_1_->AddRoute(connected_table_, "1.1.2.3/32");
            agent_a_2_->AddRoute(connected_table_, "1.1.2.3/32");
        }
        task_util::WaitForIdle();
    }


    void DeleteConnectedRoute() {
        // Add Connected route
        if (mx_push_connected_) {
            DeleteInetRoute(mx_.get(), NULL, "blue", "1.1.2.3/32");
        } else {
            agent_a_1_->DeleteRoute(connected_table_, "1.1.2.3/32");
            agent_a_2_->DeleteRoute(connected_table_, "1.1.2.3/32");
        }
    }

    string FileRead(const string &filename) {
        ifstream file(filename.c_str());
        string content((istreambuf_iterator<char>(file)),
                       istreambuf_iterator<char>());
        return content;
    }

    std::auto_ptr<autogen::ServiceChainInfo> 
        GetChainConfig(std::string filename) {
        std::auto_ptr<autogen::ServiceChainInfo> 
            params (new autogen::ServiceChainInfo());
        string content = FileRead(filename);
        istringstream sstream(content);
        xml_document xdoc;
        xml_parse_result result = xdoc.load(sstream);
        if (!result) {
            BGP_WARN_UT("Unable to load XML document. (status="
                << result.status << ", offset=" << result.offset << ")");
            assert(0);
        }
        xml_node node = xdoc.first_child(); 
        params->XmlParse(node);
        return params;
    }

    void SetServiceChainProperty(BgpServerTest *server, 
                                 std::auto_ptr<autogen::ServiceChainInfo> params) {
        // Service Chain Info
        ifmap_test_util::IFMapMsgPropertyAdd(server->config_db(), "routing-instance", 
                                             "blue-i1", 
                                             "service-chain-information", 
                                             params.release(),
                                             0);
        task_util::WaitForIdle();
    }

    void RemoveServiceChainProperty(BgpServerTest *server) {
        ifmap_test_util::IFMapMsgPropertyDelete(server->config_db(), "routing-instance", 
                                                "blue-i1", 
                                                "service-chain-information");
        task_util::WaitForIdle();
    }

    std::vector<uint32_t> GetSGIDListFromRoute(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        assert(ext_comm);
        std::vector<uint32_t> list;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_security_group(comm))
                continue;
            SecurityGroup security_group(comm);

            list.push_back(security_group.security_group_id());
        }
        std::sort(list.begin(), list.end());
        return list;
    }

    std::set<std::string> GetTunnelEncapListFromRoute(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        assert(ext_comm);
        std::set<std::string> list;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_tunnel_encap(comm))
                continue;
            TunnelEncap encap(comm);

            list.insert(TunnelEncapType::TunnelEncapToString(encap.tunnel_encap()));
        }
        return list;
    }

    std::string GetOriginVnFromRoute(BgpServerTest *server, const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        assert(ext_comm);
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_origin_vn(comm))
                continue;
            OriginVn origin_vn(comm);
            return server->routing_instance_mgr()->GetVirtualNetworkByVnIndex(origin_vn.vn_index());
        }
        return "unresolved";
    }

    EventManager evm_;
    ServerThread thread_;
    auto_ptr<BgpServerTest> cn1_;
    auto_ptr<BgpServerTest> cn2_;
    auto_ptr<BgpServerTest> mx_;
    XmppServer *cn1_xmpp_server_;
    XmppServer *cn2_xmpp_server_;
    boost::scoped_ptr<test::NetworkAgentMock> agent_a_1_;
    boost::scoped_ptr<test::NetworkAgentMock> agent_b_1_;
    boost::scoped_ptr<test::NetworkAgentMock> agent_a_2_;
    boost::scoped_ptr<test::NetworkAgentMock> agent_b_2_;
    boost::scoped_ptr<BgpXmppChannelManagerMock> bgp_channel_manager_cn1_;
    boost::scoped_ptr<BgpXmppChannelManagerMock> bgp_channel_manager_cn2_;
    const char *connected_table_;
    bool aggregate_enable_;
    bool mx_push_connected_;
};
typedef std::tr1::tuple<bool, bool, bool> TestParams;

class ServiceIntergrationParamTest :
    public ServiceChainTest,
    public ::testing::WithParamInterface<TestParams> {
    virtual void SetUp() {
        connected_table_ =
            std::tr1::get<0>(GetParam()) ? "blue-i1" : "blue";
        aggregate_enable_ = std::tr1::get<1>(GetParam());
        mx_push_connected_ = std::tr1::get<2>(GetParam());
        ServiceChainTest::SetUp();
    }

    virtual void TearDown() {
        ServiceChainTest::TearDown();
    }
};

TEST_P(ServiceIntergrationParamTest, Basic) {
    agent_a_1_->AddRoute("red", "192.168.1.1/32");
    agent_a_2_->AddRoute("red", "192.168.1.1/32");
    task_util::WaitForIdle();

    // Add Connected route
    AddConnectedRoute();
    task_util::WaitForIdle();

    if (aggregate_enable_) {
        // Check for aggregated route
        TASK_UTIL_WAIT_NE_NO_MSG(InetRouteLookup(cn1_.get(), "blue", "192.168.1.0/24"),
                                 NULL, 1000, 10000, 
                                 "Wait for Aggregate route in blue..");
        TASK_UTIL_WAIT_NE_NO_MSG(InetRouteLookup(cn2_.get(), "blue", "192.168.1.0/24"),
                                 NULL, 1000, 10000, 
                                 "Wait for Aggregate route in blue..");
    } else {
        // Check for aggregated route
        TASK_UTIL_WAIT_NE_NO_MSG(InetRouteLookup(cn1_.get(), "blue", "192.168.1.1/32"),
                                 NULL, 1000, 10000, 
                                 "Wait for Aggregate route in blue..");
        TASK_UTIL_WAIT_NE_NO_MSG(InetRouteLookup(cn2_.get(), "blue", "192.168.1.1/32"),
                                 NULL, 1000, 10000, 
                                 "Wait for Aggregate route in blue..");
    }
 
    agent_a_1_->DeleteRoute("red", "192.168.1.1/32");
    agent_a_2_->DeleteRoute("red", "192.168.1.1/32");

    DeleteConnectedRoute();
}

INSTANTIATE_TEST_CASE_P(Instance, ServiceIntergrationParamTest,
        ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool()));

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpServerTest::GlobalSetUp();
}

static void TearDown() {
    task_util::WaitForIdle();
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
