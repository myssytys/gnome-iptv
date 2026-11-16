#include "application.hpp"
#include "services/epg_manager.hpp"
#include "services/m3u_parser.hpp"
#include "services/stream_player.hpp"
#include "services/stream_recorder.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <iostream>
#include <string>
#include <vector>

namespace {

int run_self_test() {
    std::cout << "[Self-Test] 1. Testing HTTPS M3U Playlist Parser...\n";
    const std::string sample_m3u =
        "#EXTM3U url-tvg=\"https://epg.example.com/guide.xml.gz\"\n"
        "#EXTINF:-1 tvg-id=\"yle1.fi\" tvg-name=\"YLE TV1 HD\" tvg-chno=\"1\" group-title=\"Finland\",YLE TV1 HD\n"
        "https://stream.example.com/live/yle1/index.m3u8\n"
        "#EXTINF:-1 tvg-id=\"yle2.fi\" tvg-name=\"YLE TV2 HD\" tvg-chno=\"2\" group-title=\"Finland\",YLE TV2 HD\n"
        "https://stream.example.com/live/yle2/index.m3u8\n";

    auto m3u_res = iptv::M3uParser::parse_content(sample_m3u, "https://example.com/iptv.m3u");
    if (!m3u_res.success || m3u_res.channels.size() != 2 ||
        m3u_res.discovered_epg_url != "https://epg.example.com/guide.xml.gz") {
        std::cerr << "[FAIL] M3U parser check failed!\n";
        return 1;
    }
    std::cout << "  -> Parsed " << m3u_res.channels.size()
              << " channels, discovered EPG URL: " << m3u_res.discovered_epg_url << "\n";

    std::cout << "[Self-Test] 1b. Testing Xtream Codes get.php -> xmltv.php Auto-Discovery & tvg-ID Case-Insensitivity...\n";
    const std::string xtream_m3u =
        "#EXTM3U\n"
        "#EXTINF:-1 tvg-ID=\"yletv1.fi\" tvg-name=\"FI: Yle TV1 FHD [Multi-Sub]\" group-title=\"Finland - Kansalliset\",FI: Yle TV1 FHD [Multi-Sub]\n"
        "http://example.iptv:8080/demo_user/demo_pass/278607\n";
    auto xtream_res = iptv::M3uParser::parse_content(
        xtream_m3u,
        "http://example.iptv:8080/get.php?username=demo_user&password=demo_pass&type=m3u_plus&output=ts");
    if (!xtream_res.success || xtream_res.channels.size() != 1 ||
        xtream_res.channels[0].id != "yletv1.fi" ||
        xtream_res.discovered_epg_url != "http://example.iptv:8080/xmltv.php?username=demo_user&password=demo_pass") {
        std::cerr << "[FAIL] Xtream Codes get.php / tvg-ID check failed! id=" << xtream_res.channels[0].id
                  << " epg=" << xtream_res.discovered_epg_url << "\n";
        return 1;
    }
    std::cout << "  -> Extracted tvg-ID: '" << xtream_res.channels[0].id
              << "', auto-derived EPG URL: " << xtream_res.discovered_epg_url << "\n";

    std::cout << "[Self-Test] 2. Testing Direct Single-Channel .m3u8 Stream Detection...\n";
    const std::string sample_hls =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXTINF:6.000,\n"
        "segment0.ts\n";
    auto hls_res = iptv::M3uParser::parse_content(sample_hls, "https://cdn.example.com/channel1.m3u8");
    if (!hls_res.success || hls_res.channels.size() != 1 ||
        hls_res.channels[0].stream_url != "https://cdn.example.com/channel1.m3u8") {
        std::cerr << "[FAIL] Direct HLS .m3u8 detection failed!\n";
        return 1;
    }
    std::cout << "  -> Direct stream URL mapped to playable channel: " << hls_res.channels[0].stream_url << "\n";

    std::cout << "[Self-Test] 2b. Testing Local .m3u File Loading via M3uParser::fetch_and_parse_async...\n";
    std::string local_m3u_path = "/tmp/gnome-iptv-local-test.m3u";
    g_file_set_contents(local_m3u_path.c_str(), sample_m3u.c_str(), static_cast<gssize>(sample_m3u.size()), nullptr);
    iptv::M3uParser parser_instance;
    bool local_loaded = false;
    size_t local_count = 0;
    parser_instance.fetch_and_parse_async(local_m3u_path, [&](iptv::M3uParseResult r) {
        local_loaded = r.success;
        local_count = r.channels.size();
    });
    if (!local_loaded || local_count != 2) {
        g_unlink(local_m3u_path.c_str());
        std::cerr << "[FAIL] Local .m3u file loading failed!\n";
        return 1;
    }
    std::cout << "  -> Loaded " << local_count << " channels from local file " << local_m3u_path << "\n";

    std::cout << "[Self-Test] 2c. Testing On-Demand M3U File-Offset Indexer & Scroller Batch Loading...\n";
    auto idx = iptv::M3uParser::index_file(local_m3u_path, local_m3u_path);
    auto slots = iptv::M3uParser::filter_slots(idx, 0, "");
    auto page1 = iptv::M3uParser::load_channels_range(idx, slots, 0, 1);
    auto page2 = iptv::M3uParser::load_channels_range(idx, slots, 1, 1);
    g_unlink(local_m3u_path.c_str());
    if (!idx.success || slots.size() != 2 || page1.size() != 1 || page2.size() != 1 ||
        page1[0].name != "YLE TV1 HD" || page2[0].name != "YLE TV2 HD") {
        std::cerr << "[FAIL] On-demand M3U indexer / range loader failed!\n";
        return 1;
    }
    std::cout << "  -> Indexed " << idx.slots.size()
              << " slots (16 bytes/slot) and loaded batches on demand ('"
              << page1[0].name << "', '" << page2[0].name << "')\n";

    std::cout << "[Self-Test] 3. Testing XMLTV EPG Parser & SQLite Store...\n";
    std::time_t now = std::time(nullptr);
    std::tm tm_start{}, tm_mid{}, tm_end{};
    std::time_t t1 = now - 600;
    std::time_t t2 = now + 1200;
    std::time_t t3 = now + 3000;
    gmtime_r(&t1, &tm_start);
    gmtime_r(&t2, &tm_mid);
    gmtime_r(&t3, &tm_end);

    auto fmt_xmltv = [](const std::tm& tm_val) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S +0000", &tm_val);
        return std::string(buf);
    };

    std::string xmltv =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<tv>\n"
        "  <channel id=\"yle1.fi\"><display-name>YLE TV1 HD</display-name></channel>\n"
        "  <channel id=\"unlisted.tv\"><display-name>Unlisted Foreign TV</display-name></channel>\n"
        "  <programme start=\"" + fmt_xmltv(tm_start) + "\" stop=\"" + fmt_xmltv(tm_mid) + "\" channel=\"yle1.fi\">\n"
        "    <title>Evening News</title>\n"
        "    <desc>Live national and international news broadcast.</desc>\n"
        "    <category>News</category>\n"
        "  </programme>\n"
        "  <programme start=\"" + fmt_xmltv(tm_mid) + "\" stop=\"" + fmt_xmltv(tm_end) + "\" channel=\"yle1.fi\">\n"
        "    <title>Nordic Documentary</title>\n"
        "    <desc>Exploring the archipelago.</desc>\n"
        "    <category>Documentary</category>\n"
        "  </programme>\n"
        "  <programme start=\"" + fmt_xmltv(tm_start) + "\" stop=\"" + fmt_xmltv(tm_end) + "\" channel=\"unlisted.tv\">\n"
        "    <title>Should Be Filtered Out</title>\n"
        "    <desc>Not in the M3U playlist.</desc>\n"
        "  </programme>\n"
        "</tv>\n";

    std::string tmp_db = "/tmp/gnome-iptv-selftest.db";
    g_unlink(tmp_db.c_str());
    iptv::EpgManager epg(tmp_db);
    epg.set_active_channels(m3u_res.channels);
    auto epg_st = epg.load_from_memory(xmltv);
    if (!epg_st.success || epg_st.programmes_parsed != 3) {
        std::cerr << "[FAIL] EPG load_from_memory failed: " << epg_st.message << "\n";
        return 1;
    }

    auto m3u_epg_results = epg.search_programmes("", 100);
    if (epg.total_programmes() != 2 || m3u_epg_results.size() != 2 ||
        m3u_epg_results[0].channel_name != "YLE TV1 HD") {
        std::cerr << "[FAIL] EPG M3U-only filtering failed (got " << m3u_epg_results.size()
                  << " results, total=" << epg.total_programmes() << ")!\n";
        return 1;
    }

    iptv::EpgProgramme now_p{}, next_p{};
    if (!epg.get_now_and_next(m3u_res.channels[0], now_p, next_p) ||
        now_p.title != "Evening News" || next_p.title != "Nordic Documentary") {
        std::cerr << "[FAIL] EPG Now & Next lookup failed!\n";
        return 1;
    }
    std::cout << "  -> EPG Now: '" << now_p.title << "', Next: '" << next_p.title
              << "' (filtered out non-M3U channel, " << m3u_epg_results.size() << " active M3U EPG items)\n";
    g_unlink(tmp_db.c_str());

    std::cout << "[Self-Test] 4. Verifying FFmpeg Reconnect Command Generation...\n";
    auto rec_args = iptv::StreamRecorder::build_ffmpeg_record_args(
        "https://stream.example.com/live/yle1/index.m3u8", "/tmp/out.ts", "mpegts");
    bool has_reconnect = false;
    for (const auto& a : rec_args) {
        if (a == "-reconnect_streamed") has_reconnect = true;
    }
    if (!has_reconnect) {
        std::cerr << "[FAIL] FFmpeg reconnect flags missing!\n";
        return 1;
    }
    std::cout << "  -> FFmpeg reconnect flags verified.\n";

    std::cout << "[Self-Test] 5. Testing Live FFmpeg Recording & Auto-Reconnect on Stream Break...\n";
    std::string test_src = "/tmp/gnome-iptv-test-src.ts";
    g_unlink(test_src.c_str());
    const char* gen_argv[] = {
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc=duration=1:size=320x240:rate=15",
        "-c:v", "mpeg2video", "-f", "mpegts", test_src.c_str(), nullptr
    };
    GSubprocess* gen_proc = g_subprocess_newv(gen_argv, G_SUBPROCESS_FLAGS_NONE, nullptr);
    if (gen_proc) {
        g_subprocess_wait(gen_proc, nullptr, nullptr);
        g_object_unref(gen_proc);
    }

    iptv::AppConfig test_cfg;
    test_cfg.recording_dir = "/tmp/gnome-iptv-rec-test";
    test_cfg.recording_format = "mkv";
    test_cfg.auto_reconnect = true;
    test_cfg.reconnect_delay_seconds = 1;
    test_cfg.watchdog_timeout_seconds = 4;

    iptv::StreamRecorder recorder;
    recorder.configure(test_cfg);

    iptv::Channel test_ch;
    test_ch.id = "test.ch";
    test_ch.name = "TestStream";
    test_ch.stream_url = test_src;

    if (!recorder.start_recording(test_ch, "LiveTest")) {
        std::cerr << "[FAIL] Failed to start StreamRecorder!\n";
        return 1;
    }

    // Run GLib main context for ~2.5 seconds so the 1s stream finishes ("breaks")
    // and the supervisor automatically reconnects to the same URL!
    GMainContext* ctx = g_main_context_default();
    gint64 deadline = g_get_monotonic_time() + 2600000;
    while (g_get_monotonic_time() < deadline) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(20000);
    }

    auto active_stats = recorder.current_stats();
    recorder.stop_recording();
    auto final_stats = recorder.current_stats();
    g_unlink(test_src.c_str());

    if (active_stats.bytes_written == 0 || active_stats.reconnect_attempts < 1) {
        std::cerr << "[FAIL] Expected bytes_written > 0 and reconnect_attempts >= 1, got bytes="
                  << active_stats.bytes_written << ", reconnects=" << active_stats.reconnect_attempts << "\n";
        return 1;
    }
    if (!final_stats.output_path.ends_with(".mkv") ||
        !g_file_test(final_stats.output_path.c_str(), G_FILE_TEST_EXISTS)) {
        std::cerr << "[FAIL] Expected remuxed .mkv file to exist at " << final_stats.output_path << "\n";
        return 1;
    }
    std::cout << "  -> Recorded " << active_stats.bytes_written << " bytes across "
              << (active_stats.reconnect_attempts + 1) << " connection(s) (auto-reconnected "
              << active_stats.reconnect_attempts << " time(s) after stream break) & remuxed to -> "
              << final_stats.output_path << "\n";
    g_unlink(final_stats.output_path.c_str());

    std::cout << "[Self-Test] ALL TESTS PASSED.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    g_set_prgname("org.gnome.IptvPlayer");
    g_set_application_name("GNOME IPTV Player");

    std::string cli_m3u;
    std::string cli_epg;
    std::vector<char*> gtk_argv;
    gtk_argv.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--self-test") {
            return run_self_test();
        } else if (arg == "--m3u" && i + 1 < argc) {
            cli_m3u = argv[++i];
        } else if (arg == "--epg" && i + 1 < argc) {
            cli_epg = argv[++i];
        } else {
            gtk_argv.push_back(argv[i]);
        }
    }

    int filtered_argc = static_cast<int>(gtk_argv.size());
    iptv::Application app(cli_m3u, cli_epg);
    return app.run(filtered_argc, gtk_argv.data());
}
