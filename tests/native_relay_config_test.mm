// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <cassert>
#include <iostream>
#include "cara/native_mac/relay_config.h"
int main() {
    Network::RelayClient::Config c;
    auto parse=[&](std::string_view s){return NativeMac::ParseRelayConfig(s,c);};
    assert(parse(R"({"room":"test","password":"fixture-only"})"));
    assert(c.endpoint=="wss://carap2p.therad.ninja/relay" && c.room=="test" && c.password=="fixture-only");
    assert(parse(R"({"endpoint":"WSS://example.com/relay","room":"test","password":"fixture-only"})"));
    for (auto input : {"", "null", "[]", "{", R"({"room":"test"})", R"({"room":1,"password":"x"})",
         R"({"room":"test","password":"x","extra":"x"})",R"({"room":"","password":"x"})",
         R"({"room":"a\u0000b","password":"x"})",R"({"room":"a","password":"x\u0000y"})"}) assert(!parse(input));
    for (auto endpoint : {"http://example.com/relay","wss://example.com/", "wss://a@example.com/relay",
         "wss://example.com/relay?", "wss://example.com/relay#", "wss:///relay"}) {
        const std::string input=std::string("{\"room\":\"test\",\"password\":\"x\",\"endpoint\":\"")+endpoint+"\"}";
        assert(!parse(input));
    }
    assert(!parse(std::string(16385,' ')));
    assert(!parse("{\"room\":\""+std::string(65,'a')+"\",\"password\":\"x\"}"));
    assert(!parse("{\"room\":\"a\",\"password\":\""+std::string(513,'x')+"\"}"));
    std::cout << "PASS native relay private configuration validation\n";
}
