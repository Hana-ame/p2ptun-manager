#include "../src/core.hpp"
#include "../src/process_manager.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace p2ptun;

int main() {
    // 1. 测试傻瓜式端口自动补全与随机 ID/Secret 生成
    assert(NormalizeEndpoint("3389") == "127.0.0.1:3389");
    assert(NormalizeEndpoint(":2222") == "127.0.0.1:2222");
    assert(NormalizeEndpoint("127.0.0.1:1080") == "127.0.0.1:1080");
    assert(NormalizeEndpoint("192.168.1.10:8080") == "192.168.1.10:8080");
    assert(ExtractPortOnly("127.0.0.1:3389") == "3389");
    assert(SuggestConnectPort("127.0.0.1:3389") == "13389");
    assert(SuggestConnectPort("127.0.0.1:22") == "2222");
    assert(SuggestConnectPort("127.0.0.1:1080") == "1080");
    assert(SuggestExposePort("127.0.0.1:13389") == "3389");
    assert(SuggestExposePort("127.0.0.1:2222") == "22");
    assert(SuggestExposePort("127.0.0.1:17890") == "7890");
    const char* flip_pairs[][2] = {
        {"3389", "13389"}, {"22", "2222"}, {"80", "18080"}, {"443", "18443"},
        {"445", "10445"}, {"3306", "13306"}, {"5432", "15432"}, {"7890", "17890"},
    };
    for (auto& pr : flip_pairs) {
        std::string expose_ep = std::string("127.0.0.1:") + pr[0];
        std::string flipped = SuggestConnectPort(expose_ep);
        assert(flipped == pr[1]);
        assert(SuggestExposePort("127.0.0.1:" + flipped) == pr[0]);
    }

    // 验证默认随机 ID 与随机 Secret 生成
    std::string rid1 = GenerateRandomTunnelId();
    std::string rid2 = GenerateRandomTunnelId();
    assert(!rid1.empty() && rid1.rfind("tun-", 0) == 0);
    assert(rid1 != rid2);
    std::string rsec1 = GenerateRandomSecret();
    assert(!rsec1.empty() && rsec1.rfind("p2p-", 0) == 0);

    // 验证空规则规范化时自动赋予随机 ID，且 id 与 name 一致
    TunnelRule emptyIdRule;
    emptyIdRule.port = "3389";
    emptyIdRule = NormalizeRule(emptyIdRule, "expose", "hy2", 100);
    assert(!emptyIdRule.id.empty());
    assert(emptyIdRule.id == emptyIdRule.name);
    assert(emptyIdRule.port == "127.0.0.1:3389");

    // 2. 测试一键配对码 (p2ptun://) 生成与解析
    TunnelRule expRule;
    expRule.mode = "expose";
    expRule.remark = "家里远程桌面";
    expRule.name = "home-rdp";
    expRule.port = "3389";
    expRule.proto = "hy2";
    expRule.bandwidth_mbps = 100;
    expRule = NormalizeRule(expRule, "expose", "hy2", 100);
    assert(expRule.id == "home-rdp");

    std::string uri = BuildPairingUri(expRule, "MySecret888");
    std::cout << "[PASS] Generated Pairing URI: " << uri << "\n";

    TunnelRule parsedFromUri;
    std::string err;
    bool ok = ParsePairingInput(uri, parsedFromUri, err);
    assert(ok);
    assert(parsedFromUri.mode == "connect");
    assert(parsedFromUri.id == "home-rdp");
    assert(parsedFromUri.name == "home-rdp");
    assert(parsedFromUri.port == "127.0.0.1:13389");
    assert(parsedFromUri.proto == "hy2");
    assert(parsedFromUri.bandwidth_mbps == 100);
    assert(parsedFromUri.enabled == true);

    // 3. 测试从 CLI 命令导入配对
    std::string cli = BuildCliCommand(expRule, "MySecret888");
    std::cout << "[PASS] Generated CLI Command: " << cli << "\n";
    TunnelRule parsedFromCli;
    ok = ParsePairingInput(cli, parsedFromCli, err);
    assert(ok);
    assert(parsedFromCli.mode == "connect");
    assert(parsedFromCli.id == "home-rdp");
    assert(parsedFromCli.name == "home-rdp");
    assert(parsedFromCli.port == "127.0.0.1:13389");

    // 4. 测试极简口令导入 (name:secret@port)
    TunnelRule parsedShort;
    ok = ParsePairingInput("office-ssh:hunter2@2222", parsedShort, err);
    assert(ok);
    assert(parsedShort.mode == "connect");
    assert(parsedShort.id == "office-ssh");
    assert(parsedShort.name == "office-ssh");
    assert(parsedShort.port == "127.0.0.1:2222");

    // 4.5 测试简易模式最小配置 (仅 secret + 每个 item 仅 id 和 port)
    {
        std::string simpleJson = R"({
            "secret": "shared-secret-all",
            "tunnels": [
                { "id": "tun-alpha", "port": "3389" },
                { "id": "tun-beta", "port": "22" }
            ]
        })";
        AppConfig simpleCfg;
        assert(ParseAppConfigFromJson(simpleJson, simpleCfg, err));
        assert(simpleCfg.default_secret == "shared-secret-all");
        assert(simpleCfg.tunnels.size() == 2);
        assert(simpleCfg.tunnels[0].id == "tun-alpha");
        assert(simpleCfg.tunnels[0].name == "tun-alpha");
        assert(simpleCfg.tunnels[0].port == "127.0.0.1:3389");
        assert(EffectiveSecret(simpleCfg.tunnels[0], simpleCfg) == "shared-secret-all");
        assert(EffectiveSecret(simpleCfg.tunnels[1], simpleCfg) == "shared-secret-all");

        // 简易配置序列化后不冗余输出无关字段
        std::string dumped = SerializeAppConfigToJson(simpleCfg);
        assert(dumped.find("\"secret\": \"shared-secret-all\"") != std::string::npos);
        assert(dumped.find("\"id\": \"tun-alpha\"") != std::string::npos);
        assert(dumped.find("\"port\": \"127.0.0.1:3389\"") != std::string::npos);
        assert(dumped.find("\"remark\"") == std::string::npos);
        assert(dumped.find("\"name\"") == std::string::npos);
    }

    // 5. 测试多端口配置文件序列化与反序列化 (含注释、旧格式迁移与单条 secret 覆盖)
    std::string sampleJson = R"({
        // 傻瓜式全局配置
        "default_secret": "global-pass-999",
        "default_proto": "hy2",
        "default_bandwidth": 150,
        "expose": [
            { "remark": "RDP", "name": "pc-rdp", "port": "3389", "enabled": true },
            { "remark": "SSH", "name": "pc-ssh", "port": "22", "enabled": false }
        ],
        "connect": [
            { "remark": "连公司RDP", "name": "office-rdp", "port": "13389", "enabled": true },
            { "remark": "连家里代理", "name": "home-proxy", "port": "1080", "secret": "override-sec", "enabled": true }
        ]
    })";

    AppConfig cfg;
    ok = ParseAppConfigFromJson(sampleJson, cfg, err);
    if (!ok) {
        std::cerr << "ParseAppConfigFromJson failed: " << err << "\n";
        return 1;
    }
    assert(cfg.default_secret == "global-pass-999");
    assert(cfg.tunnels.size() == 4);
    assert(cfg.expose.empty());
    assert(cfg.connect.empty());
    assert(cfg.tunnels[0].port == "127.0.0.1:3389");
    assert(cfg.tunnels[0].last_role == "expose");
    assert(cfg.tunnels[1].port == "127.0.0.1:22");
    assert(cfg.tunnels[2].port == "127.0.0.1:13389");
    assert(cfg.tunnels[2].last_role == "connect");
    assert(EffectiveSecret(cfg) == cfg.default_secret);
    assert(EffectiveSecret(cfg.tunnels[0], cfg) == "global-pass-999"); // 默认全等
    assert(EffectiveSecret(cfg.tunnels[3], cfg) == "override-sec");    // 高级模式单条覆盖

    // 验证子进程命令行参数绝不包含明文密钥
    auto subArgs = BuildSubprocessArgs(cfg.tunnels[0], cfg, 18301);
    for (const auto& a : subArgs) {
        assert(a != "-secret");
        assert(a != "global-pass-999");
    }

    // 6. 测试 p2ptun /status JSON 快照解析
    std::string statusJson = R"({
        "version": "0.5.2",
        "pid": 33458,
        "mode": "expose",
        "peer_id": "pc-rdp",
        "endpoint": "127.0.0.1:3389",
        "proto": "hy2",
        "udp_endpoint": "183.193.27.110:1570",
        "broker": "wss://0.peerjs.com",
        "uptime_sec": 360,
        "uptime": "6m0s",
        "signaling_up": true,
        "active_peers": 2,
        "active_streams": 5,
        "reconnects": 1,
        "bytes_tx": 10485760,
        "bytes_rx": 5242880,
        "last_rtt_ms": 16
    })";
    StatusSnapshot snap;
    assert(ParseStatusSnapshotJson(statusJson, snap));
    assert(snap.reachable);
    assert(snap.pid == 33458);
    assert(snap.signaling_up == true);
    assert(snap.active_peers == 2);
    assert(snap.active_streams == 5);
    assert(snap.last_rtt_ms == 16);
    assert(snap.udp_endpoint == "183.193.27.110:1570");
    assert(FormatBytes(snap.bytes_tx) == "10.0 MB");

    // 6.5 全量 flag 构造与全局高级默认值回退
    {
        p2ptun::TunnelRule ex;
        ex.mode = "expose";
        ex.id = "flag-rule";
        ex.port = "3389";
        ex.max_peers = 32;
        ex.force_relay = true;
        ex.keep_proxy = true;
        ex.broker = "wss://b.example.com";
        ex.peer_key = "peerjs";
        ex = p2ptun::NormalizeRule(ex, "expose", "hy2", 100);

        p2ptun::AppConfig g;
        auto args2 = p2ptun::BuildSubprocessArgs(ex, g, 18400);
        auto contains = [&args2](const std::string& v) {
            for (const auto& a : args2) if (a == v) return true;
            return false;
        };
        assert(contains("expose"));
        assert(contains("flag-rule"));
        assert(contains("-max-peers")); assert(contains("32"));
        assert(contains("-force-relay"));
        assert(contains("-keep-proxy"));
        assert(contains("-broker"));
        assert(contains("wss://b.example.com"));
        assert(!contains("-key"));
        assert(contains("-status-addr")); assert(contains("127.0.0.1:18400"));
        for (const auto& a : args2) assert(a != "global-pass-999");

        p2ptun::AppConfig g2;
        g2.adv_keepalive = "20s";
        g2.adv_broker = "wss://global-broker.example.com";
        g2.adv_max_peers = 8;
        p2ptun::TunnelRule blank;
        blank.mode = "expose";
        blank.id = "blank-rule";
        blank.port = "8080";
        blank = p2ptun::NormalizeRule(blank, "expose", "hy2", 100);
        auto args4 = p2ptun::BuildSubprocessArgs(blank, g2, 18500);
        auto has4 = [&args4](const std::string& v) {
            for (const auto& a : args4) if (a == v) return true;
            return false;
        };
        assert(has4("-keepalive")); assert(has4("20s"));
        assert(has4("-broker")); assert(has4("wss://global-broker.example.com"));
        assert(has4("-max-peers")); assert(has4("8"));
        auto args5 = p2ptun::BuildSubprocessArgs(ex, g2, 18501);
        auto has5 = [&args5](const std::string& v) {
            for (const auto& a : args5) if (a == v) return true;
            return false;
        };
        assert(has5("-broker") && has5("wss://b.example.com"));
        assert(has5("-keepalive") && has5("20s"));

        p2ptun::TunnelRule cn;
        cn.mode = "connect";
        cn.id = "flag-rule-c";
        cn.port = "13389";
        cn.on_demand = true;
        cn.idle_timeout = "30m";
        cn = p2ptun::NormalizeRule(cn, "connect", "hy2", 100);
        auto args3 = p2ptun::BuildSubprocessArgs(cn, g, 18401);
        auto contains3 = [&args3](const std::string& v) {
            for (const auto& a : args3) if (a == v) return true;
            return false;
        };
        assert(contains3("-on-demand"));
        assert(contains3("-idle-timeout")); assert(contains3("30m"));
        assert(!contains3("-max-peers"));
        assert(!contains3("-force-relay"));
    }

    // 7. 测试 MultiTunnelManager 多端口增删改、导入配对码与启停生命周期
    std::string tmpCfgPath = (p2ptun::Trim(std::getenv("TEMP") ? std::getenv("TEMP") : "") != "")
                                 ? std::string(std::getenv("TEMP")) + "/p2ptun_manager_test_config.json"
                                 : "/tmp/p2ptun_manager_test_config.json";
    std::remove(tmpCfgPath.c_str());
    {
        MultiTunnelManager mgr(tmpCfgPath);
        auto initialRows = mgr.GetRowViews();
        assert(initialRows.size() == 3);
        for (const auto& r : initialRows) {
            assert(!r.id.empty() && r.id.rfind("tun-", 0) == 0);
        }

        std::string newId, mgrErr;
        assert(mgr.ImportFromPairingCode("my-nas:naspass123@5000", newId, mgrErr));
        auto afterImport = mgr.GetRowViews();
        assert(afterImport.size() == 4);

        assert(mgr.DeleteRule(newId, mgrErr));
        assert(mgr.GetRowViews().size() == 3);

        // 高级参数序列化往返
        p2ptun::TunnelRule adv;
        adv.mode = "expose";
        adv.remark = "高级参数规则";
        adv.id = "adv-rule";
        adv.port = "3389";
        adv.broker = "wss://broker1.example.com,wss://broker2.example.com";
        adv.stun = "stun:stun.example.com:3478";
        adv.turn = "turn:user:pass@turn.example.com:3478?transport=udp";
        adv.proxy = "http://127.0.0.1:7890";
        adv.peer_key = "custom-peerjs-key";
        adv.keepalive = "15s";
        adv.ice_ports = "40000-40100";
        adv.nat1to1 = "203.0.113.10";
        adv.idle_timeout = "30m";
        adv.max_peers = 32;
        adv.force_relay = true;
        adv.keep_proxy = true;
        adv.auto_restart = false;
        std::string outId2;
        assert(mgr.AddOrUpdateRule(adv, false, outId2, mgrErr));

        p2ptun::AppConfig reloaded;
        std::string loadErr;
        assert(p2ptun::LoadAppConfigFromFile(mgr.ConfigPath(), reloaded, loadErr));
        bool found = false;
        for (const auto& r : reloaded.tunnels) {
            if (r.id == outId2) {
                found = true;
                assert(r.broker == adv.broker);
                assert(r.stun == adv.stun);
                assert(r.turn == adv.turn);
                assert(r.proxy == adv.proxy);
                assert(r.peer_key == adv.peer_key);
                assert(r.keepalive == "15s");
                assert(r.ice_ports == "40000-40100");
                assert(r.nat1to1 == "203.0.113.10");
                assert(r.idle_timeout == "30m");
                assert(r.max_peers == 32);
                assert(r.force_relay == true);
                assert(r.keep_proxy == true);
                assert(r.auto_restart == false);
            }
        }
        assert(found);
    }
    std::remove(tmpCfgPath.c_str());

    std::cout << "[PASS] All C++17 core & MultiTunnelManager unit tests succeeded!\n";
    return 0;
}
