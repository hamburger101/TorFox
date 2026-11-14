// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/core/browsing/shortcuts.h"
#include "src/core/crypto/os_crypto.h"
#include "src/core/tor/bridges.h"
#include "src/core/tor/control.h"
#include "src/core/tor/controller.h"
#include "src/core/tor/onion.h"
#include "src/core/tor/torrc.h"
#include "src/core/url/guards.h"

namespace torfox {
namespace {

using tor::Transport;

constexpr char kObfs4[] =
    "obfs4 37.218.245.14:38224 D9A82D2F9C2F65A18407B1D2B764F130847F8B5D "
    "cert=bjRaMrr1BRiAW8IE9U5z27fQaYgOhX1UCmOpg2pFpoMvo6ZgQMzLsaTzzQNTlm7hNcb+Sg iat-mode=0";

// ----- Bridge lines ------------------------------------------------------------

TEST(TorBridges, AcceptsTheLinesTheTorProjectHandsOut) {
  std::string error;
  auto b = tor::ParseBridgeLine(kObfs4, &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->transport, Transport::kObfs4);
  EXPECT_EQ(b->line, kObfs4);

  b = tor::ParseBridgeLine("  Bridge   webtunnel [2001:db8:7d2e:9a81::1]:443 "
                           "8E4C34C5BD8E3BB3C4B4F74E2D71C9B6A4F5D6E7 url=https://example.com/abc ver=0.0.1",
                           &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->transport, Transport::kWebTunnel);
  EXPECT_EQ(b->line.rfind("webtunnel [2001:db8:7d2e:9a81::1]:443 8E4C", 0), 0u);

  b = tor::ParseBridgeLine(
      "snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
      "fingerprint=2B280B23E1107BB62ABFC40DDCC8824814F80A72 url=https://1098762253.rsc.cdn77.org/ "
      "fronts=app.datapacket.com,www.datapacket.com ice=stun:stun.epygi.com:3478 utls-imitate=hellorandomizedalpn",
      &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->transport, Transport::kSnowflake);

  b = tor::ParseBridgeLine("192.0.2.10:9001 D9A82D2F9C2F65A18407B1D2B764F130847F8B5D", &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->transport, Transport::kVanilla);
  b = tor::ParseBridgeLine("meek_lite 192.0.2.20:80 url=https://example.net/ front=www.example.org",
                           &error);
  ASSERT_TRUE(b) << error;
  EXPECT_EQ(b->transport, Transport::kMeekLite);
}

TEST(TorBridges, NothingButABridgeReachesTorrc) {
  // Each of these could add an option, a comment or a program to torrc, or
  // is simply not a bridge.
  const char* bad[] = {
      "obfs4 192.0.2.1:443 D9A82D2F9C2F65A18407B1D2B764F130847F8B5D cert=x\nExitNodes {us}",
      "obfs4 192.0.2.1:443 D9A82D2F9C2F65A18407B1D2B764F130847F8B5D cert=x\rUseBridges 0",
      "obfs4 192.0.2.1:443 cert=abc#comment",
      "obfs4 192.0.2.1:443 cert=\"quoted\"",
      "obfs4 192.0.2.1:443 cert=a\\b",
      "obfs4 192.0.2.1:443 CERT=abc",
      "obfs4 192.0.2.1:443 =abc",
      "obfs4 192.0.2.1:443 cert=",
      "obfs4 192.0.2.1:443 justaword",
      "conjure 192.0.2.1:443",
      "obfs3 192.0.2.1:443",
      "ClientTransportPlugin obfs4 exec C:\\evil.exe",
      "obfs4 example.com:443",
      "obfs4 192.0.2.1",
      "obfs4 192.0.2.1:0",
      "obfs4 192.0.2.1:65536",
      "obfs4 256.0.2.1:443",
      "obfs4 01.0.2.1:443",
      "obfs4 [2001:db8::1:443",
      "obfs4 [2001:db8:::1]:443",
      "192.0.2.10:9001 D9A82D2F9C2F65A18407B1D2B764F130847F8B5",
      "192.0.2.10:9001 D9A82D2F9C2F65A18407B1D2B764F130847F8B5D cert=abc",
      "obfs4 192.0.2.1:443 cert=\xc3\xa9",
      "",
      "Bridge",
  };
  for (const char* line : bad) {
    std::string error;
    EXPECT_FALSE(tor::ParseBridgeLine(line, &error)) << line;
    EXPECT_FALSE(error.empty()) << line;
  }
  EXPECT_FALSE(tor::ParseBridgeLine(std::string("obfs4 192.0.2.1:443 cert=") + std::string(3000, 'a'),
                                    nullptr));
}

TEST(TorBridges, CustomListReportsEachBadLine) {
  const std::string text = std::string("# my bridges\r\n\r\n") + kObfs4 +
                           "\n  \nobfs4 192.0.2.1:443 cert=a#b\n192.0.2.10:9001\n";
  tor::BridgeList list = tor::ParseBridgeLines(text);
  ASSERT_EQ(list.bridges.size(), 2u);
  EXPECT_EQ(list.bridges[0].line, kObfs4);
  EXPECT_EQ(list.bridges[1].line, "192.0.2.10:9001");
  ASSERT_EQ(list.errors.size(), 1u);
  EXPECT_EQ(list.errors[0].rfind("Line 5: ", 0), 0u) << list.errors[0];

  std::string many;
  for (int i = 0; i < 70; ++i) {
    many += "192.0.2." + std::to_string(i + 1) + ":443\n";
  }
  list = tor::ParseBridgeLines(many);
  EXPECT_EQ(list.bridges.size(), tor::kMaxBridges);
  EXPECT_FALSE(list.errors.empty());
}

TEST(TorBridges, BuiltinBridgesComeFromTheBundle) {
  const std::string config = std::string(R"({"bridges": {"obfs4": [")") + kObfs4 +
                             R"("], "snowflake": [], "meek": ["meek_lite 192.0.2.20:80 url=https://e.net/ front=e.org"]}})";
  std::string error;
  auto obfs4 = tor::BuiltinBridges(config, "obfs4", &error);
  ASSERT_EQ(obfs4.size(), 1u) << error;
  EXPECT_EQ(obfs4[0].line, kObfs4);
  EXPECT_EQ(tor::BuiltinBridges(config, "meek", &error).size(), 1u);
  EXPECT_TRUE(tor::BuiltinBridges(config, "snowflake", &error).empty());
  EXPECT_TRUE(tor::BuiltinBridges(config, "conjure", &error).empty());
  EXPECT_TRUE(tor::BuiltinBridges("{not json", "obfs4", &error).empty());
  // One bad line spoils the set: TorFox never writes part of a list it doubts.
  const std::string bad = R"({"bridges": {"obfs4": ["obfs4 192.0.2.1:443 cert=x\nExitNodes {us}"]}})";
  EXPECT_TRUE(tor::BuiltinBridges(bad, "obfs4", &error).empty());
}

// ----- torrc --------------------------------------------------------------------

tor::TorrcOptions Options() {
  tor::TorrcOptions o;
  o.data_dir = std::filesystem::path("C:/Users/A B/TorFox/Tor");
  o.control_port_file = o.data_dir / "control-port";
  o.owning_pid = 4242;
  o.pt_client = "pluggable_transports/lyrebird.exe";
  return o;
}

bool HasLine(const std::string& torrc, const std::string& line) {
  return ("\n" + torrc).find("\n" + line + "\n") != std::string::npos;
}

TEST(Torrc, ListensOnLoopbackOnlyWithCookieAuthAndExitsWithTorFox) {
  std::string error;
  const std::string t = tor::BuildTorrc(Options(), &error);
  ASSERT_FALSE(t.empty()) << error;
  EXPECT_TRUE(HasLine(t, "SocksPort 127.0.0.1:auto ExtendedErrors IsolateDestAddr")) << t;
  EXPECT_TRUE(HasLine(t, "ControlPort 127.0.0.1:auto")) << t;
  EXPECT_TRUE(HasLine(t, "CookieAuthentication 1")) << t;
  EXPECT_TRUE(HasLine(t, "__OwningControllerProcess 4242")) << t;
  EXPECT_TRUE(HasLine(t, "ClientOnly 1")) << t;
  EXPECT_TRUE(HasLine(t, "DataDirectory \"C:/Users/A B/TorFox/Tor\"")) << t;
  EXPECT_TRUE(HasLine(t, "Log notice stdout")) << t;
  EXPECT_EQ(t.find("UseBridges"), std::string::npos);
  EXPECT_EQ(t.find("ClientTransportPlugin"), std::string::npos);
  EXPECT_EQ(t.find("HashedControlPassword"), std::string::npos);
  // Nothing listens anywhere but loopback.
  EXPECT_EQ(t.find("0.0.0.0"), std::string::npos);
}

TEST(Torrc, BridgesBringTheirTransport) {
  tor::TorrcOptions o = Options();
  o.bridges.push_back(*tor::ParseBridgeLine(kObfs4, nullptr));
  std::string error;
  std::string t = tor::BuildTorrc(o, &error);
  ASSERT_FALSE(t.empty()) << error;
  EXPECT_TRUE(HasLine(t, "UseBridges 1"));
  EXPECT_TRUE(HasLine(t, std::string("Bridge ") + kObfs4));
  EXPECT_TRUE(HasLine(t, "ClientTransportPlugin meek_lite,obfs4,webtunnel exec pluggable_transports/lyrebird.exe"));
  // A bridge that bypassed parsing is still refused.
  o.bridges.push_back(tor::Bridge{Transport::kObfs4, "obfs4 192.0.2.1:443 cert=a\nExitNodes {us}"});
  EXPECT_TRUE(tor::BuildTorrc(o, &error).empty());
  // A transport program path that torrc would split or that leaves the folder.
  o = Options();
  o.bridges.push_back(*tor::ParseBridgeLine(kObfs4, nullptr));
  for (const char* pt : {"", "pluggable transports/lyrebird.exe", "../lyrebird.exe", "C:\\pt\\lyrebird.exe",
                         "/usr/bin/lyrebird"}) {
    o.pt_client = pt;
    EXPECT_TRUE(tor::BuildTorrc(o, &error).empty()) << pt;
  }
  // Plain bridge relays need no transport.
  o.pt_client.clear();
  o.bridges = {*tor::ParseBridgeLine("192.0.2.10:9001", nullptr)};
  t = tor::BuildTorrc(o, &error);
  ASSERT_FALSE(t.empty()) << error;
  EXPECT_EQ(t.find("ClientTransportPlugin"), std::string::npos);
}

TEST(Torrc, TheNoticeLogLineIsQuotedAsAWhole) {
  // Found by tests/e2e/test_tor.py RealTorTest: tor refused
  // 'Log notice file "C:\\..."' (the quotes became part of the file name).
  tor::TorrcOptions o = Options();
  o.notice_log = std::filesystem::path("C:\\Users\\A B\\Tor\\tor-notices.log");
  std::string error;
  const std::string t = tor::BuildTorrc(o, &error);
  ASSERT_FALSE(t.empty()) << error;
  EXPECT_TRUE(HasLine(t, "Log \"notice file C:\\\\Users\\\\A B\\\\Tor\\\\tor-notices.log\"")) << t;
  EXPECT_EQ(t.find("Log notice stdout"), std::string::npos);
}

TEST(Torrc, PathsAreQuotedOrRefused) {
  EXPECT_EQ(tor::QuotePath(std::filesystem::path("C:\\a \"b\"\\c")), "\"C:\\\\a \\\"b\\\"\\\\c\"");
  EXPECT_EQ(tor::QuotePath(std::filesystem::path(u8"C:/Users/Jos\u00e9")), "");
  EXPECT_EQ(tor::QuotePath(std::filesystem::path("C:/a\nb")), "");
  tor::TorrcOptions o = Options();
  o.data_dir = std::filesystem::path(u8"C:/Users/Jos\u00e9/TorFox");
  std::string error;
  EXPECT_TRUE(tor::BuildTorrc(o, &error).empty());
  EXPECT_FALSE(error.empty());
}

// ----- Control protocol ---------------------------------------------------------

TEST(TorControl, ReadsMultiLineDataAndAsyncReplies) {
  tor::ReplyReader r;
  r.Feed("250-PROTOCOLINFO 1\r\n250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"C:\\\\x\"\r\n");
  EXPECT_FALSE(r.Next());
  r.Feed("250 OK\r\n650 STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=5 TAG=conn SUMMARY=\"Connecting\"\r\n"
         "250+config-text=\r\nSocksPort 9050\r\n..dotted\r\n.\r\n250 O");
  auto a = r.Next();
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->ok());
  ASSERT_EQ(a->lines.size(), 3u);
  EXPECT_EQ(a->lines[1], "AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"C:\\\\x\"");
  auto e = r.Next();
  ASSERT_TRUE(e);
  EXPECT_TRUE(e->async());
  EXPECT_FALSE(r.Next());
  r.Feed("K\r\n");
  auto d = r.Next();
  ASSERT_TRUE(d);
  ASSERT_EQ(d->lines.size(), 2u);
  EXPECT_EQ(d->lines[0], "config-text=\nSocksPort 9050\n.dotted\n");
  EXPECT_EQ(d->lines[1], "OK");
  EXPECT_FALSE(r.failed());
}

TEST(TorControl, MalformedOrHugeRepliesEndTheConnection) {
  for (const char* bad : {"25 OK\r\n", "abc OK\r\n", "250xOK\r\n", "250-a\r\n251 b\r\n"}) {
    tor::ReplyReader r;
    r.Feed(bad);
    EXPECT_TRUE(r.failed()) << bad;
    EXPECT_FALSE(r.Next()) << bad;
  }
  tor::ReplyReader r;
  r.Feed(std::string(tor::ReplyReader::kMaxLine + 10, 'a'));
  EXPECT_TRUE(r.failed());
  tor::ReplyReader big;
  for (int i = 0; i < 40 && !big.failed(); ++i) {
    big.Feed("250-" + std::string(30000, 'x') + "\r\n");
  }
  EXPECT_TRUE(big.failed());
}

TEST(TorControl, ParsesKeywordsBootstrapAndPorts) {
  auto kw = tor::ParseKeywords("BOOTSTRAP PROGRESS=45 TAG=loading SUMMARY=\"Loading \\\"relay\\\" info\" X=");
  EXPECT_EQ(kw["PROGRESS"], "45");
  EXPECT_EQ(kw["SUMMARY"], "Loading \"relay\" info");
  EXPECT_TRUE(kw.count("BOOTSTRAP"));
  EXPECT_EQ(kw["X"], "");

  auto b = tor::ParseBootstrap(
      "status/bootstrap-phase=NOTICE BOOTSTRAP PROGRESS=100 TAG=done SUMMARY=\"Done\"");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->progress, 100);
  EXPECT_EQ(b->tag, "done");
  EXPECT_TRUE(b->warning.empty());
  b = tor::ParseBootstrap(
      "STATUS_CLIENT WARN BOOTSTRAP PROGRESS=10 TAG=conn_done SUMMARY=\"Connected to a relay\" "
      "WARNING=\"Connection refused\" REASON=CONNECTREFUSED COUNT=3 RECOMMENDATION=warn");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->progress, 10);
  EXPECT_EQ(b->warning, "Connection refused");
  EXPECT_FALSE(tor::ParseBootstrap("STATUS_CLIENT NOTICE CIRCUIT_ESTABLISHED"));
  EXPECT_FALSE(tor::ParseBootstrap("NOTICE BOOTSTRAP PROGRESS=101 TAG=x"));

  EXPECT_EQ(tor::ParseControlPortFile("PORT=127.0.0.1:9151\n"), 9151);
  EXPECT_EQ(tor::ParseControlPortFile("PORT=127.0.0.1:9151\r\n"), 9151);
  EXPECT_FALSE(tor::ParseControlPortFile("PORT=0.0.0.0:9151\n"));
  EXPECT_FALSE(tor::ParseControlPortFile("PORT=192.0.2.1:9151\n"));
  EXPECT_FALSE(tor::ParseControlPortFile("PORT=127.0.0.1:99999\n"));
  EXPECT_FALSE(tor::ParseControlPortFile("UNIX_PORT=/tmp/x\n"));
  EXPECT_EQ(tor::ParseLoopbackListener("\"127.0.0.1:53211\""), 53211);
  EXPECT_EQ(tor::ParseLoopbackListener("\"[::1]:9050\" \"127.0.0.1:9050\""), 9050);
  EXPECT_FALSE(tor::ParseLoopbackListener("\"0.0.0.0:9050\""));
  EXPECT_EQ(tor::QuoteString("a\"b\\c\nd"), "\"a\\\"b\\\\c\\nd\"");
}

TEST(TorCrypto, HmacSha256MatchesRfc4231) {
  // RFC 4231 test cases 1 and 2.
  auto h1 = crypto::HmacSha256(std::string(20, '\x0b'), "Hi There");
  ASSERT_TRUE(h1);
  EXPECT_EQ(crypto::HexEncode(std::string(reinterpret_cast<const char*>(h1->data()), h1->size())),
            "B0344C61D8DB38535CA8AFCEAF0BF12B881DC200C9833DA726E9376C2E32CFF7");
  auto h2 = crypto::HmacSha256("Jefe", "what do ya want for nothing?");
  ASSERT_TRUE(h2);
  EXPECT_EQ(crypto::HexEncode(std::string(reinterpret_cast<const char*>(h2->data()), h2->size())),
            "5BDCC146BF60754E6A042426089575C75A003F089D2739839DEC58B964EC3843");
  EXPECT_EQ(crypto::RandomBytes(32).size(), 32u);
  EXPECT_NE(crypto::RandomBytes(32), crypto::RandomBytes(32));
  EXPECT_EQ(crypto::HexDecode("00ff10"), std::string("\x00\xff\x10", 3));
  EXPECT_FALSE(crypto::HexDecode("0"));
  EXPECT_FALSE(crypto::HexDecode("zz"));
  EXPECT_TRUE(crypto::ConstantTimeEqual("abc", "abc"));
  EXPECT_FALSE(crypto::ConstantTimeEqual("abc", "abd"));
  EXPECT_FALSE(crypto::ConstantTimeEqual("abc", "ab"));
}

// ----- Controller, against a scripted tor ---------------------------------------

// Plays tor's side of the control protocol. |honest| tors compute the server
// hash from the real cookie; dishonest ones (another program on the port)
// cannot.
class FakeTor : public tor::Channel {
 public:
  FakeTor(std::string cookie, bool honest) : cookie_(std::move(cookie)), honest_(honest) {}

  bool Send(std::string_view bytes) override {
    std::string cmd(bytes);
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
      cmd.pop_back();
    }
    received.push_back(cmd);
    if (cmd == "PROTOCOLINFO 1") {
      Out("250-PROTOCOLINFO 1\r\n250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"x\"\r\n"
          "250-VERSION Tor=\"0.4.9.3\"\r\n250 OK\r\n");
    } else if (cmd.rfind("AUTHCHALLENGE SAFECOOKIE ", 0) == 0) {
      client_nonce_ = *crypto::HexDecode(cmd.substr(25));
      server_nonce_ = std::string(32, '\x42');
      auto h = tor::SafeCookieHashes(honest_ ? cookie_ : std::string(32, 'x'), client_nonce_,
                                     server_nonce_);
      Out("250 AUTHCHALLENGE SERVERHASH=" + crypto::HexEncode(h->server_hash) +
          " SERVERNONCE=" + crypto::HexEncode(server_nonce_) + "\r\n");
    } else if (cmd.rfind("AUTHENTICATE ", 0) == 0) {
      auto h = tor::SafeCookieHashes(cookie_, client_nonce_, server_nonce_);
      Out(cmd.substr(13) == crypto::HexEncode(h->client_hash) ? "250 OK\r\n"
                                                               : "515 Authentication failed\r\n");
    } else if (cmd == "TAKEOWNERSHIP" || cmd == "SETEVENTS STATUS_CLIENT" || cmd == "SIGNAL NEWNYM") {
      Out("250 OK\r\n");
    } else if (cmd == "GETINFO net/listeners/socks") {
      Out("250-net/listeners/socks=\"127.0.0.1:53211\"\r\n250 OK\r\n");
    } else if (cmd == "GETINFO status/bootstrap-phase") {
      Out("250-status/bootstrap-phase=NOTICE BOOTSTRAP PROGRESS=5 TAG=conn SUMMARY=\"Connecting\"\r\n"
          "250 OK\r\n");
      Out("650 STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=100 TAG=done SUMMARY=\"Done\"\r\n");
    } else {
      Out("510 Unrecognized command\r\n");
    }
    return true;
  }
  std::string Receive() override {
    if (out_.empty()) {
      return std::string();
    }
    std::string s = out_.front();
    out_.pop_front();
    return s;
  }
  void Close() override {}

  std::vector<std::string> received;

 private:
  void Out(std::string s) { out_.push_back(std::move(s)); }
  std::string cookie_;
  bool honest_;
  std::string client_nonce_, server_nonce_;
  std::deque<std::string> out_;
};

TEST(TorController, AuthenticatesTakesOwnershipAndReadsState) {
  const std::string cookie(32, '\x07');
  auto fake = std::make_unique<FakeTor>(cookie, true);
  FakeTor* t = fake.get();
  tor::Controller c(std::move(fake));
  tor::Controller::SetupResult r = c.Setup(cookie);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.socks_port, 53211);
  EXPECT_EQ(r.bootstrap.progress, 5);
  EXPECT_EQ(r.tor_version, "0.4.9.3");
  const std::vector<std::string> order = {"PROTOCOLINFO 1", "AUTHCHALLENGE", "AUTHENTICATE",
                                          "TAKEOWNERSHIP", "SETEVENTS STATUS_CLIENT",
                                          "GETINFO net/listeners/socks", "GETINFO status/bootstrap-phase"};
  ASSERT_EQ(t->received.size(), order.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    EXPECT_EQ(t->received[i].rfind(order[i], 0), 0u) << t->received[i];
  }
  // The event that arrived after setup is delivered by Run().
  int progress = -1;
  c.Run([&](const tor::Reply& e) {
    if (auto b = tor::ParseBootstrap(e.lines.front())) progress = b->progress;
  });
  EXPECT_EQ(progress, 100);
}

TEST(TorController, NeverAnswersAPortThatCannotProveItKnowsTheCookie) {
  const std::string cookie(32, '\x07');
  auto fake = std::make_unique<FakeTor>(cookie, false);
  FakeTor* t = fake.get();
  tor::Controller c(std::move(fake));
  tor::Controller::SetupResult r = c.Setup(cookie);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("did not prove"), std::string::npos) << r.error;
  for (const std::string& cmd : t->received) {
    EXPECT_NE(cmd.rfind("AUTHENTICATE", 0), 0u) << "sent " << cmd;
  }
  // A cookie of the wrong size is refused before anything is derived from it.
  auto fake2 = std::make_unique<FakeTor>(cookie, true);
  tor::Controller c2(std::move(fake2));
  EXPECT_FALSE(c2.Setup("short").ok);
}

TEST(TorController, CommandsAreRefusedBeforeRunAndCannotCarryNewlines) {
  tor::Controller c(std::make_unique<FakeTor>(std::string(32, '\x07'), true));
  EXPECT_FALSE(c.Command("SIGNAL NEWNYM", nullptr));
  EXPECT_FALSE(c.Command("SIGNAL NEWNYM\r\nSETCONF ExitNodes={us}", nullptr));
}

TEST(TorController, ConnectLoopbackFailsCleanlyWithNothingListening) {
  std::string error;
  // Port 9 (discard) is not open on test machines; a refused connection
  // must come back as an error, not hang.
  EXPECT_EQ(tor::ConnectLoopback(9, 2000, &error), nullptr);
  EXPECT_FALSE(error.empty());
}

// ----- Onion addresses ---------------------------------------------------------

std::string Hex(const std::array<uint8_t, 32>& d) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : d) {
    out += kDigits[b >> 4];
    out += kDigits[b & 15];
  }
  return out;
}

TEST(TorOnion, Sha3MatchesFips202Vectors) {
  EXPECT_EQ(Hex(tor::Sha3_256("")),
            "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
  EXPECT_EQ(Hex(tor::Sha3_256("abc")),
            "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
  // Longer than one 136-byte block.
  EXPECT_EQ(Hex(tor::Sha3_256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
                              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
                              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            Hex(tor::Sha3_256(std::string(
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))));
  EXPECT_EQ(Hex(tor::Sha3_256(std::string(200, 'a'))).size(), 64u);
  EXPECT_EQ(Hex(tor::Sha3_256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376");
}

TEST(TorOnion, ChecksVersion3AddressesIncludingTheirChecksum) {
  const std::string ddg(tor::kDuckDuckGoOnion);
  EXPECT_EQ(tor::CheckOnionHost(ddg), tor::OnionCheck::kValid);
  EXPECT_EQ(tor::CheckOnionHost(ddg + "."), tor::OnionCheck::kValid);
  EXPECT_EQ(tor::CheckOnionHost("www." + ddg), tor::OnionCheck::kValid);
  // One character changed: the checksum catches it.
  std::string typo = ddg;
  typo[20] = typo[20] == 'a' ? 'b' : 'a';
  EXPECT_EQ(tor::CheckOnionHost(typo), tor::OnionCheck::kBadChecksum);
  EXPECT_EQ(tor::CheckOnionHost("expyuzz4wqqyqhjn.onion"), tor::OnionCheck::kOldVersion);
  EXPECT_EQ(tor::CheckOnionHost("abc.onion"), tor::OnionCheck::kBadLength);
  EXPECT_EQ(tor::CheckOnionHost(std::string(56, '1') + ".onion"), tor::OnionCheck::kBadCharacters);
  EXPECT_EQ(tor::CheckOnionHost(std::string(56, 'a') + ".onion"), tor::OnionCheck::kBadVersion);
  EXPECT_EQ(tor::CheckOnionHost("example.com"), tor::OnionCheck::kNotOnion);
  EXPECT_EQ(tor::CheckOnionHost(".onion"), tor::OnionCheck::kNotOnion);
  for (tor::OnionCheck c : {tor::OnionCheck::kNotOnion, tor::OnionCheck::kOldVersion,
                            tor::OnionCheck::kBadLength, tor::OnionCheck::kBadCharacters,
                            tor::OnionCheck::kBadVersion, tor::OnionCheck::kBadChecksum}) {
    EXPECT_FALSE(tor::OnionCheckMessage(c).empty());
  }
}

TEST(TorOnion, TypedOnionAddressesBecomeUrlsAndAreNeverSearched) {
  const std::string ddg(tor::kDuckDuckGoOnion);
  EXPECT_EQ(tor::OnionUrlFromInput(ddg).url, "http://" + ddg);
  EXPECT_EQ(tor::OnionUrlFromInput("  " + ddg + "/a?b=c ").url, "http://" + ddg + "/a?b=c");
  EXPECT_EQ(tor::OnionUrlFromInput("https://" + ddg + "/").url, "https://" + ddg + "/");
  EXPECT_EQ(tor::OnionUrlFromInput("HTTP://" + ddg).url, "HTTP://" + ddg);
  for (const std::string& bad :
       {"what is " + ddg, "ftp://" + ddg, "javascript://" + ddg, std::string("abc.onion"),
        "http://" + ddg.substr(1), "http://example.com/" + ddg}) {
    tor::OnionInput in = tor::OnionUrlFromInput(bad);
    EXPECT_TRUE(in.url.empty()) << bad;
    EXPECT_FALSE(in.error.empty()) << bad;
  }
}

// ----- Tor tabs' navigation policy ---------------------------------------------

TEST(TorGuards, TorTabsAllowOnionHostsAndRefuseLocalAddresses) {
  const std::string onion = "http://" + std::string(tor::kDuckDuckGoOnion) + "/";
  using guards::Role;
  EXPECT_FALSE(guards::NavigationBlockReason(Role::kWebTab, onion).empty());
  EXPECT_TRUE(guards::NavigationBlockReason(Role::kTorTab, onion).empty());
  EXPECT_FALSE(guards::ResourceBlockReason(Role::kWebTab, onion).empty());
  EXPECT_TRUE(guards::ResourceBlockReason(Role::kTorTab, onion).empty());
  EXPECT_TRUE(guards::NavigationBlockReason(Role::kTorTab, "https://example.com/").empty());
  for (const char* url :
       {"http://localhost/", "http://a.localhost:8080/", "http://127.0.0.1/", "http://10.1.2.3/",
        "http://192.168.0.1/", "http://172.16.0.1/", "http://172.31.255.255/",
        "http://169.254.1.1/", "http://100.64.0.1/", "http://0.0.0.0/", "http://[::1]/",
        "http://[fe80::1]/", "http://[fd00::1]/", "http://[::ffff:7f00:1]/",
        "http://[::ffff:127.0.0.1]/", "http://[::]/", "https://LOCALHOST./"}) {
    EXPECT_FALSE(guards::NavigationBlockReason(Role::kTorTab, url).empty()) << url;
    EXPECT_FALSE(guards::ResourceBlockReason(Role::kTorTab, url).empty()) << url;
    // Normal web tabs may still reach the user's own network.
    EXPECT_TRUE(guards::NavigationBlockReason(Role::kWebTab, url).empty()) << url;
  }
  for (const char* url : {"http://172.32.0.1/", "http://100.128.0.1/", "http://11.0.0.1/",
                          "http://[2001:db8::1]/", "http://192.169.0.1/", "http://notlocalhost/"}) {
    EXPECT_TRUE(guards::NavigationBlockReason(Role::kTorTab, url).empty()) << url;
  }
  // Privileged schemes stay refused in Tor tabs too.
  EXPECT_FALSE(guards::NavigationBlockReason(Role::kTorTab, "torfox://settings/").empty());
  EXPECT_FALSE(guards::NavigationBlockReason(Role::kTorTab, "file:///C:/").empty());
}

TEST(TorShortcuts, NewTorWindowFollowsEachPlatformsConvention) {
  using browsing::Command;
  using browsing::KeyPlatform;
  namespace km = browsing::keymod;
  auto win = browsing::MapShortcut('N', km::kAlt | km::kShift, KeyPlatform::kWindows);
  ASSERT_TRUE(win);
  EXPECT_EQ(win->command, Command::kNewTorWindow);
  auto mac = browsing::MapShortcut('N', km::kCmd | km::kAlt, KeyPlatform::kMac);
  ASSERT_TRUE(mac);
  EXPECT_EQ(mac->command, Command::kNewTorWindow);
  // Ctrl+Alt is AltGr on many keyboards: never a shortcut.
  EXPECT_FALSE(browsing::MapShortcut('N', km::kCtrl | km::kAlt, KeyPlatform::kWindows));
}

}  // namespace
}  // namespace torfox
