#include "../src/core.hpp"
#include "../src/process_manager.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace p2ptun;

int main() {
    // 1. 测试傻瓜式端口自动补全
    assert(NormalizeEndpoint("3389") == "127.0.0.1:3389");
    assert(NormalizeEndpoint(":2222") == "127.0.0.1:2222");
    assert(NormalizeEndpoint("127.0.0.1:1080") == "127.0.0.1:1080");
    assert(NormalizeEndpoint("192.168.1.10:8080") == "192.168.1.10:8080");
    assert(ExtractPortOnly("127.0.0.1:3389") == "3389");
    assert(SuggestConnectPort("127.0.0.1:3389") == "13389");
    assert(SuggestConnectPort("127.0.0.1:22") == "2222");
    assert(SuggestConnectPort("127.0.0.1:1080") == "1080");

    // 2. 测试一键配对码 (p2ptun://) 生成与解析
    TunnelRule expRule;
    expRule.mode = "expose";
    expRule.remark = "家里远程桌面";
    expRule.name = "home-rdp";
    expRule.port = "3389";
    expRule.proto = "hy2";
    expRule.bandwidth_mbps = 100;
    expRule = NormalizeRule(expRule, "expose", "hy2", 100);

    std::string uri = BuildPairingUri(expRule, "MySecret888");
    std::cout << "[PASS] Generated Pairing URI: " << uri << "\n";

    TunnelRule parsedFromUri;
    std::string err;
    bool ok = ParsePairingInput(uri, parsedFromUri, err);
    assert(ok);
    assert(parsedFromUri.mode == "connect");
    assert(parsedFromUri.name == "home-rdp");
    assert(parsedFromUri.secret == "MySecret888");
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
    assert(parsedFromCli.name == "home-rdp");
    assert(parsedFromCli.secret == "MySecret888");
    assert(parsedFromCli.port == "127.0.0.1:13389");

    // 4. 测试极简口令导入 (name:secret@port)
    TunnelRule parsedShort;
    ok = ParsePairingInput("office-ssh:hunter2@2222", parsedShort, err);
    assert(ok);
    assert(parsedShort.mode == "connect");
    assert(parsedShort.name == "office-ssh");
    assert(parsedShort.secret == "hunter2");
    assert(parsedShort.port == "127.0.0.1:2222");

    // 5. 测试多端口配置文件序列化与反序列化 (含注释与纯数字端口)
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
    assert(cfg.expose.size() == 2);
    assert(cfg.connect.size() == 2);
    assert(cfg.expose[0].port == "127.0.0.1:3389");
    assert(cfg.expose[1].port == "127.0.0.1:22");
    assert(cfg.connect[0].port == "127.0.0.1:13389");
    assert(EffectiveSecret(cfg.connect[0], cfg) == "global-pass-999");
    assert(EffectiveSecret(cfg.connect[1], cfg) == "override-sec");

    // 验证子进程命令行参数绝不包含明文密钥
    auto subArgs = BuildSubprocessArgs(cfg.expose[0], cfg, 18301);
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

    // 6.5 串口参数解析与全量 flag 构造 (新增合并能力)
    {
        int db = 0; char par = 0; int sb = 0;
        assert(p2ptun::ParseSerialParams("8N1", db, par, sb)); assert(db == 8 && par == 'N' && sb == 1);
        assert(p2ptun::ParseSerialParams("7E2", db, par, sb)); assert(db == 7 && par == 'E' && sb == 2);
        assert(p2ptun::ParseSerialParams("8O1", db, par, sb)); assert(par == 'O');
        assert(!p2ptun::ParseSerialParams("9X3", db, par, sb));
        assert(!p2ptun::ParseSerialParams("", db, par, sb));

        p2ptun::TunnelRule ex;
        ex.mode = "expose";
        ex.name = "flag-rule";
        ex.port = "3389";
        ex.max_peers = 32;
        ex.force_relay = true;
        ex.keep_proxy = true;
        ex.broker = "wss://b.example.com";
        ex.peer_key = "peerjs"; // 默认值，不应传 -key
        ex = p2ptun::NormalizeRule(ex, "expose", "hy2", 100);

        p2ptun::AppConfig g;
        auto args2 = p2ptun::BuildSubprocessArgs(ex, g, 18400);
        auto contains = [&args2](const std::string& v) {
            for (const auto& a : args2) if (a == v) return true;
            return false;
        };
        assert(contains("expose"));
        assert(contains("-max-peers")); assert(contains("32"));
        assert(contains("-force-relay"));
        assert(contains("-keep-proxy"));
        assert(contains("-broker"));
        assert(contains("wss://b.example.com"));
        assert(!contains("-key")); // peerjs 默认 key 不应出现在命令行
        assert(contains("-status-addr")); assert(contains("127.0.0.1:18400"));
        for (const auto& a : args2) assert(a != "global-pass-999");

        p2ptun::TunnelRule cn;
        cn.mode = "connect";
        cn.name = "flag-rule-c";
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
        assert(!contains3("-max-peers")); // connect 模式不传 -max-peers
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
        assert(initialRows.size() == 4); // 默认 2 条 expose + 2 条 connect

        std::string newId, mgrErr;
        assert(mgr.ImportFromPairingCode("my-nas:naspass123@5000", newId, mgrErr));
        auto afterImport = mgr.GetRowViews();
        assert(afterImport.size() == 5);

        // 注意: 不在此处调用 ToggleTunnel/StartTunnel —— 启动会拉起真实子进程,
        // CI 环境没有 p2ptun.exe 会以错误弹窗路径失败。仅验证规则增删与序列化往返。
        // (启动生命周期由 Windows 上的 smoke 流程与人工验收覆盖)
        assert(mgr.DeleteRule(newId, mgrErr));
        assert(mgr.GetRowViews().size() == 4);

        // 高级参数序列化往返 (新增合并能力验证)
        p2ptun::TunnelRule adv;
        adv.mode = "expose";
        adv.remark = "高级参数规则";
        adv.name = "adv-rule";
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
        adv.serial_port = "COM3";
        adv.serial_baud = 9600;
        adv.serial_params = "7E2";
        std::string outId2;
        assert(mgr.AddOrUpdateRule(adv, false, outId2, mgrErr));

        p2ptun::AppConfig reloaded;
        std::string loadErr;
        assert(p2ptun::LoadAppConfigFromFile(mgr.ConfigPath(), reloaded, loadErr));
        bool found = false;
        for (const auto& r : reloaded.expose) {
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
                assert(r.serial_port == "COM3");
                assert(r.serial_baud == 9600);
                assert(r.serial_params == "7E2");
            }
        }
        assert(found);
    }
    std::remove(tmpCfgPath.c_str());

    std::cout << "[PASS] All C++17 core & MultiTunnelManager unit tests succeeded!\n";
    return 0;
}
