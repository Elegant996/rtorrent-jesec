// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

#define __STDC_FORMAT_MACROS

#include "buildinfo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <inttypes.h>
#include <iostream>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <torrent/buildinfo.h>
#include <torrent/data/chunk_utils.h>
#include <torrent/exceptions.h>
#include <torrent/http.h>
#include <torrent/poll.h>
#include <torrent/torrent.h>
#include <torrent/utils/error_number.h>
#include <torrent/utils/log.h>

#ifdef LT_HAVE_BACKTRACE
#include <execinfo.h>
#endif

#include "core/dht_manager.h"
#include "core/download.h"
#include "core/download_factory.h"
#include "core/download_store.h"
#include "core/manager.h"
#include "core/view_manager.h"
#include "display/canvas.h"
#include "display/manager.h"
#include "display/window.h"
#include "input/bindings.h"
#include "ui/root.h"

#include "rpc/command_scheduler.h"
#include "rpc/command_scheduler_item.h"
#include "rpc/parse_commands.h"
#include "utils/directory.h"
#include "utils/indicators.h"

#include "command_helpers.h"
#include "control.h"
#include "globals.h"
#include "option_parser.h"
#include "signal_handler.h"

#include "thread_worker.h"

#define LT_LOG(log_fmt, ...)                                                   \
  lt_log_print(torrent::LOG_SYSTEM, "system: " log_fmt, __VA_ARGS__);

void
handle_sigbus(int signum, siginfo_t* sa, void* ptr);
void
do_panic(int signum);
void
print_help();
void
initialize_commands();

void
do_nothing() {}

int
parse_options(int                                              argc,
              char**                                           argv,
              std::queue<std::string>*                         cmd1,
              std::queue<std::pair<std::string, std::string>>* cmd2) {
  try {
    OptionParser optionParser;

    // Converted.
    optionParser.insert_flag('h', [](const auto&) { return print_help(); });
    optionParser.insert_flag('n', [](const auto&) { return do_nothing(); });
    optionParser.insert_flag('D', [](const auto&) { return do_nothing(); });
    optionParser.insert_flag('I', [](const auto&) { return do_nothing(); });
    optionParser.insert_flag('K', [](const auto&) { return do_nothing(); });

    optionParser.insert_option('b', [](const auto& arg) {
      return rpc::call_command_set_string("network.bind_address.set", arg);
    });
    optionParser.insert_option('d', [](const auto& arg) {
      return rpc::call_command_set_string("directory.default.set", arg);
    });
    optionParser.insert_option('i', [](const auto& arg) {
      return rpc::call_command_set_string("ip", arg);
    });
    optionParser.insert_option('p', [](const auto& arg) {
      return rpc::call_command_set_string("network.port_range.set", arg);
    });
    optionParser.insert_option('s', [](const auto& arg) {
      return rpc::call_command_set_string("session", arg);
    });

    optionParser.insert_option(
      'O', [&cmd1](const std::string& cmd) { cmd1->push(cmd); });
    optionParser.insert_option_list(
      'o', [&cmd2](const std::string& key, const std::string& arg) {
        cmd2->push(std::pair(key, arg));
      });

    return optionParser.process(argc, argv);

  } catch (torrent::input_error& e) {
    throw torrent::input_error("Failed to parse command line option: " +
                               std::string(e.what()));
  }
}

void
load_session_torrents() {
  indicators::BlockProgressBar* progress_bar = nullptr;

  utils::Directory entries =
    control->core()->download_store()->get_formated_entries();

  const auto entries_size = entries.size();

  if (!display::Canvas::isInitialized() && entries_size) {
    std::cout << "rTorrent: loading " << entries_size
              << " entries from session directory" << std::endl;
    if (isatty(fileno(stdin)) && isatty(fileno(stdout))) {
      progress_bar = new indicators::BlockProgressBar{
        indicators::option::BarWidth{ 50 },
        indicators::option::ForegroundColor{ indicators::Color::white },
        indicators::option::FontStyles{
          std::vector<indicators::FontStyle>{ indicators::FontStyle::bold } },
        indicators::option::MaxProgress{ entries_size + 1 }
      };
    }
  }

  for (const auto& entry : entries) {
    // We don't really support session torrents that are links. These
    // would be overwritten anyway on exit, and thus not really be
    // useful.
    if (!entry.is_file()) {
      if (progress_bar != nullptr) {
        progress_bar->tick();
      }
      continue;
    }

    core::DownloadFactory* f = new core::DownloadFactory(control->core());

    // Replace with session torrent flag.
    f->set_session(true);
    f->set_init_load(true);
    f->set_immediate(true);
    f->slot_finished([f, &progress_bar, entries_size]() {
      if (control->is_shutdown_received()) {
        throw std::runtime_error("shutdown received. aborting...");
      }
      if (progress_bar != nullptr) {
        progress_bar->set_option(indicators::option::PostfixText{
          std::to_string(progress_bar->current()) + "/" +
          std::to_string(entries_size) });
        progress_bar->tick();
      }
      delete f;
    });

    f->load(entries.path() + entry.d_name);
    f->commit();
  }

  // Hash torrents
  if (progress_bar != nullptr) {
    progress_bar->set_option(indicators::option::PostfixText{ "Checking" });
    progress_bar->tick();
  }

  const auto& hashing_view = *control->view_manager()->find_throw("hashing");
  control->core()->set_hashing_view(hashing_view);
  hashing_view->set_focus(hashing_view->focus());

  priority_queue_perform(&taskScheduler, cachedTime);

  if (progress_bar != nullptr) {
    progress_bar->set_option(indicators::option::PostfixText{ "" });
    progress_bar->mark_as_completed();
    delete progress_bar;
  }
}

void
load_arg_torrents(char** first, char** last) {
  for (; first != last; ++first) {
    core::DownloadFactory* f = new core::DownloadFactory(control->core());

    // Replace with session torrent flag.
    f->set_start(true);
    f->set_init_load(true);
    f->slot_finished([f]() { delete f; });
    f->load(*first);
    f->commit();
  }
}

static uint64_t
client_next_timeout() {
  if (taskScheduler.empty())
    return (control->is_shutdown_started()
              ? torrent::utils::timer::from_milliseconds(100)
              : torrent::utils::timer::from_seconds(60))
      .usec();
  else if (taskScheduler.top()->time() <= cachedTime)
    return 0;
  else
    return (taskScheduler.top()->time() - cachedTime).usec();
}

static void
client_perform() {
  // Use throw exclusively.
  if (control->is_shutdown_completed())
    throw torrent::shutdown_exception();

  if (control->is_shutdown_received())
    control->handle_shutdown();

  control->inc_tick();

  cachedTime = torrent::utils::timer::current();
  torrent::utils::priority_queue_perform(&taskScheduler, cachedTime);
}

int
main(int argc, char** argv) {
  try {

    // Temporary.
    setlocale(LC_ALL, "");

    cachedTime = torrent::utils::timer::current();

    // Initialize logging:
    torrent::log_initialize();

    control = new Control;

    // Seed RNG
    std::random_device rd;
    std::mt19937       mt(rd());

    uint uint_seed = std::uniform_int_distribution<uint>(
      std::numeric_limits<uint>::min(), std::numeric_limits<uint>::max())(mt);
    long long_seed = std::uniform_int_distribution<long>(
      std::numeric_limits<long>::min(), std::numeric_limits<long>::max())(mt);

    srandom(uint_seed);
    srand48(long_seed);

    SignalHandler::set_ignore(SIGPIPE);
    SignalHandler::set_handler(
      SIGINT, [control = control] { control->receive_normal_shutdown(); });
    SignalHandler::set_handler(
      SIGHUP, [control = control] { control->receive_normal_shutdown(); });
    SignalHandler::set_handler(
      SIGTERM, [control = control] { control->receive_quick_shutdown(); });
    SignalHandler::set_handler(
      SIGWINCH, [display = control->display()] { display->force_redraw(); });
    SignalHandler::set_handler(SIGSEGV, [] { return do_panic(SIGSEGV); });
    SignalHandler::set_handler(SIGILL, [] { return do_panic(SIGILL); });
    SignalHandler::set_handler(SIGFPE, [] { return do_panic(SIGFPE); });

    SignalHandler::set_sigaction_handler(SIGBUS, &handle_sigbus);

    // SIGUSR1 is used for interrupting polling, forcing the target
    // thread to process new non-socket events.
    //
    // LibTorrent uses sockets for this purpose on Solaris and other
    // platforms that do not properly pass signals to the target
    // threads. Use '--enable-interrupt-socket' when configuring
    // LibTorrent to enable this workaround.
    if (torrent::thread_base::should_handle_sigusr1())
      SignalHandler::set_handler(SIGUSR1, [] { return do_nothing(); });

    torrent::log_add_group_output(torrent::LOG_NOTICE, "important");
    torrent::log_add_group_output(torrent::LOG_INFO, "complete");

    torrent::Poll::slot_create_poll() = [] { return core::create_poll(); };

    torrent::initialize();
    torrent::main_thread()->slot_do_work() = [] { return client_perform(); };
    torrent::main_thread()->slot_next_timeout() = [] {
      return client_next_timeout();
    };

    worker_thread = new ThreadWorker();
    worker_thread->init_thread();

    // Initialize option handlers after libtorrent to ensure
    // torrent::ConnectionManager* are valid etc.
    initialize_commands();

    if (OptionParser::has_flag('D', argc, argv)) {
      rpc::call_command_set_value("method.use_deprecated.set", true);
      lt_log_print(torrent::LOG_WARN, "Enabled deprecated commands.");
    }

    if (OptionParser::has_flag('I', argc, argv)) {
      rpc::call_command_set_value("method.use_intermediate.set", 0);
      lt_log_print(torrent::LOG_WARN, "Disabled intermediate commands.");
    }

    if (OptionParser::has_flag('K', argc, argv)) {
      rpc::call_command_set_value("method.use_intermediate.set", 2);
      lt_log_print(torrent::LOG_WARN,
                   "Allowing intermediate commands local-only.");
    }

    rpc::parse_command_multiple(
      rpc::make_target(),
      "method.insert = event.view.hide,multi|rlookup|static\n"
      "method.insert = event.view.show,multi|rlookup|static\n"

      "method.insert = event.system.startup_done,multi|rlookup|static\n"
      "method.insert = event.system.shutdown,multi|rlookup|static\n"

      "method.insert = event.download.inserted,multi|rlookup|static\n"
      "method.insert = event.download.inserted_new,multi|rlookup|static\n"
      "method.insert = event.download.inserted_session,multi|rlookup|static\n"
      "method.insert = event.download.erased,multi|rlookup|static\n"
      "method.insert = event.download.opened,multi|rlookup|static\n"
      "method.insert = event.download.closed,multi|rlookup|static\n"
      "method.insert = event.download.resumed,multi|rlookup|static\n"
      "method.insert = event.download.paused,multi|rlookup|static\n"

      "method.insert = event.download.finished,multi|rlookup|static\n"
      "method.insert = event.download.hash_done,multi|rlookup|static\n"
      "method.insert = event.download.hash_failed,multi|rlookup|static\n"
      "method.insert = event.download.hash_final_failed,multi|rlookup|static\n"
      "method.insert = event.download.hash_removed,multi|rlookup|static\n"
      "method.insert = event.download.hash_queued,multi|rlookup|static\n"
      "method.insert = event.download.active,multi|rlookup|static\n"
      "method.insert = event.download.inactive,multi|rlookup|static\n"

      "method.set_key = event.download.inserted,         1_send_scrape, "
      "((d.tracker.send_scrape,30))\n"
      "method.set_key = event.download.inserted_new,     1_prepare, "
      "{(branch,((d.state)),((view.set_visible,started)),((view.set_visible,"
      "stopped)) ),(d.save_full_session)}\n"
      "method.set_key = event.download.inserted_session, 1_prepare, "
      "{(branch,((d.state)),((view.set_visible,started)),((view.set_visible,"
      "stopped)) )}\n"

      "method.set_key = event.download.inserted, 1_prioritize_toc, "
      "\"branch=file.prioritize_toc=,{\\\"f.multicall=(file.prioritize_toc."
      "first),f.prioritize_first.enable=\\\",\\\"f.multicall=(file.prioritize_"
      "toc.last),f.prioritize_last.enable=\\\",d.update_priorities=}\"\n"

      "method.set_key = event.download.erased, !_download_list, "
      "ui.unfocus_download=\n"
      "method.set_key = event.download.erased, ~_delete_tied, d.delete_tied=\n"

      "method.set_key = event.download.resumed,   !_timestamp, "
      "((d.timestamp.started.set_if_z, ((system.time)) ))\n"
      "method.set_key = event.download.finished,  !_timestamp, "
      "((d.timestamp.finished.set_if_z, ((system.time)) ))\n"
      "method.set_key = event.download.hash_done, !_timestamp, "
      "{(branch,((d.complete)),((d.timestamp.finished.set_if_z,(system.time))))"
      "}\n"
      "method.set_key = event.download.inactive,  !_timestamp, "
      "((d.timestamp.last_active.set, ((system.time)) ))\n"

      "method.insert.c_simple = group.insert_persistent_view,"
      "((view.add,((argument.0)))),((view.persistent,((argument.0)))),((group."
      "insert,((argument.0)),((argument.0))))\n"

      "file.prioritize_toc.first.set = {*.avi,*.mp4,*.mkv,*.gz}\n"
      "file.prioritize_toc.last.set  = {*.zip}\n"

      // Allow setting 'group2.view' as constant, so that we can't
      // modify the value. And look into the possibility of making
      // 'const' use non-heap memory, as we know they can't be
      // erased.

      // TODO: Remember to ensure it doesn't get restarted by watch
      // dir, etc. Set ignore commands, or something.

      "group.insert = seeding,seeding\n"

      "session.name.set = (cat,(system.hostname),:,(system.pid))\n"

      // Currently not doing any sorting on main.
      "view.add = main\n"
      "view.add = default\n"

      "view.add = name\n"

      "view.add = active\n"
      "view.filter = active,((false))\n"

      "view.add = started\n"
      "view.filter = started,((false))\n"
      "view.event_added   = "
      "started,{(view.set_not_visible,stopped),(d.state.set,1),(scheduler."
      "simple.added)}\n"
      "view.event_removed = "
      "started,{(view.set_visible,stopped),(scheduler.simple.removed)}\n"

      "view.add = stopped\n"
      "view.filter = stopped,((false))\n"
      "view.event_added   = "
      "stopped,{(d.state.set,0),(view.set_not_visible,started)}\n"
      "view.event_removed = stopped,((view.set_visible,started))\n"

      "view.add = complete\n"
      "view.filter = complete,((d.complete))\n"
      "view.filter_on    = "
      "complete,event.download.hash_done,event.download.hash_failed,event."
      "download.hash_final_failed,event.download.finished\n"

      "view.add = incomplete\n"
      "view.filter = incomplete,((not,((d.complete))))\n"
      "view.filter_on    = "
      "incomplete,event.download.hash_done,event.download.hash_failed,"
      "event.download.hash_final_failed,event.download.finished\n"

      // The hashing view does not include stopped torrents.
      "view.add = hashing\n"
      "view.filter = hashing,((d.hashing))\n"
      "view.filter_on = "
      "hashing,event.download.hash_queued,event.download.hash_removed,"
      "event.download.hash_done,event.download.hash_failed,event.download.hash_"
      "final_failed,event.download.finished\n"

      "view.add    = seeding\n"
      "view.filter = seeding,((and,((d.state)),((d.complete))))\n"
      "view.filter_on = "
      "seeding,event.download.resumed,event.download.paused,event.download."
      "finished\n"

      "view.add    = leeching\n"
      "view.filter = leeching,((and,((d.state)),((not,((d.complete))))))\n"
      "view.filter_on = "
      "leeching,event.download.resumed,event.download.paused,event.download."
      "finished\n"

      "schedule2 = session_save,1200,1200,((session.save))\n"
      "schedule2 = low_diskspace,5,60,((close_low_diskspace,500M))\n"
      "schedule2 = "
      "prune_file_status,3600,86400,((system.file_status_cache.prune))\n"

      "protocol.encryption.set=allow_incoming,try_outgoing,enable_retry\n");

    // Functions that might not get depracted as they are nice for
    // configuration files, and thus might do with just some
    // cleanup.
    CMD2_REDIRECT_GENERIC("upload_rate", "throttle.global_up.max_rate.set_kb");
    CMD2_REDIRECT_GENERIC("download_rate",
                          "throttle.global_down.max_rate.set_kb");

    CMD2_REDIRECT_GENERIC("ratio.enable", "group.seeding.ratio.enable");
    CMD2_REDIRECT_GENERIC("ratio.disable", "group.seeding.ratio.disable");
    CMD2_REDIRECT_GENERIC("ratio.min", "group2.seeding.ratio.min");
    CMD2_REDIRECT_GENERIC("ratio.max", "group2.seeding.ratio.max");
    CMD2_REDIRECT_GENERIC("ratio.upload", "group2.seeding.ratio.upload");
    CMD2_REDIRECT_GENERIC("ratio.min.set", "group2.seeding.ratio.min.set");
    CMD2_REDIRECT_GENERIC("ratio.max.set", "group2.seeding.ratio.max.set");
    CMD2_REDIRECT_GENERIC("ratio.upload.set",
                          "group2.seeding.ratio.upload.set");

    CMD2_REDIRECT_GENERIC("encryption", "protocol.encryption.set");
    CMD2_REDIRECT_GENERIC("encoding_list", "encoding.add");

    CMD2_REDIRECT_GENERIC("connection_leech", "protocol.connection.leech.set");
    CMD2_REDIRECT_GENERIC("connection_seed", "protocol.connection.seed.set");

    CMD2_REDIRECT("min_peers", "throttle.min_peers.normal.set");
    CMD2_REDIRECT("max_peers", "throttle.max_peers.normal.set");
    CMD2_REDIRECT("min_peers_seed", "throttle.min_peers.seed.set");
    CMD2_REDIRECT("max_peers_seed", "throttle.max_peers.seed.set");

    CMD2_REDIRECT("min_uploads", "throttle.min_uploads.set");
    CMD2_REDIRECT("max_uploads", "throttle.max_uploads.set");
    CMD2_REDIRECT("min_downloads", "throttle.min_downloads.set");
    CMD2_REDIRECT("max_downloads", "throttle.max_downloads.set");

    CMD2_REDIRECT("max_uploads_div", "throttle.max_uploads.div.set");
    CMD2_REDIRECT("max_uploads_global", "throttle.max_uploads.global.set");
    CMD2_REDIRECT("max_downloads_div", "throttle.max_downloads.div.set");
    CMD2_REDIRECT("max_downloads_global", "throttle.max_downloads.global.set");

    CMD2_REDIRECT_GENERIC("max_memory_usage", "pieces.memory.max.set");

    CMD2_REDIRECT("bind", "network.bind_address.set");
    CMD2_REDIRECT("ip", "network.local_address.set");
    CMD2_REDIRECT("port_range", "network.port_range.set");

    CMD2_REDIRECT_GENERIC("dht", "dht.mode.set");
    CMD2_REDIRECT_GENERIC("dht_port", "dht.port.set");

    CMD2_REDIRECT("port_random", "network.port_random.set");
    CMD2_REDIRECT("proxy_address", "network.proxy_address.set");

    CMD2_REDIRECT("scgi_port", "network.scgi.open_port");
    CMD2_REDIRECT("scgi_local", "network.scgi.open_local");

    CMD2_REDIRECT_GENERIC("directory", "directory.default.set");
    CMD2_REDIRECT_GENERIC("session", "session.path.set");

    CMD2_REDIRECT("check_hash", "pieces.hash.on_completion.set");

    CMD2_REDIRECT("key_layout", "keys.layout.set");

    CMD2_REDIRECT_GENERIC("to_gm_time", "convert.gm_time");
    CMD2_REDIRECT_GENERIC("to_gm_date", "convert.gm_date");
    CMD2_REDIRECT_GENERIC("to_time", "convert.time");
    CMD2_REDIRECT_GENERIC("to_date", "convert.date");
    CMD2_REDIRECT_GENERIC("to_elapsed_time", "convert.elapsed_time");
    CMD2_REDIRECT_GENERIC("to_kb", "convert.kb");
    CMD2_REDIRECT_GENERIC("to_mb", "convert.mb");
    CMD2_REDIRECT_GENERIC("to_xb", "convert.xb");
    CMD2_REDIRECT_GENERIC("to_throttle", "convert.throttle");

    CMD2_REDIRECT("torrent_list_layout", "ui.torrent_list.layout.set");

    // Deprecated commands. Don't use these anymore.

    if (rpc::call_command_value("method.use_intermediate") == 1) {
      CMD2_REDIRECT_GENERIC("execute", "execute2");

      CMD2_REDIRECT_GENERIC("schedule", "schedule2");
      CMD2_REDIRECT_GENERIC("schedule_remove", "schedule_remove2");

    } else if (rpc::call_command_value("method.use_intermediate") == 2) {
      // Allow for use in config files, etc, just don't export it.
      CMD2_REDIRECT_GENERIC_NO_EXPORT("execute", "execute2");

      CMD2_REDIRECT_GENERIC_NO_EXPORT("schedule", "schedule2");
      CMD2_REDIRECT_GENERIC_NO_EXPORT("schedule_remove", "schedule_remove2");
    }

    bool isCertFound = false;

    std::function<void(const char*)> apply_cert =
      [&isCertFound](const char* caPath) {
        if (!isCertFound && caPath != nullptr && caPath[0] == '/' &&
            access(caPath, F_OK) != -1) {
          isCertFound = true;
          rpc::parse_command_single(rpc::make_target(),
                                    "network.http.cacert.set = " +
                                      std::string(caPath));
        }
      };

    apply_cert(std::getenv("RTORRENT_CA_BUNDLE"));

#ifdef RT_USE_RUNTIME_CA_DETECTION
    apply_cert(std::getenv("CURL_CA_BUNDLE"));
    apply_cert(std::getenv("SSL_CERT_FILE"));
    apply_cert("/etc/ssl/certs/ca-certificates.crt");
    apply_cert("/etc/pki/tls/certs/ca-bundle.crt");
    apply_cert("/usr/share/ssl/certs/ca-bundle.crt");
    apply_cert("/usr/local/share/certs/ca-root-nss.crt");
    apply_cert("/etc/ssl/cert.pem");
#endif

    std::queue<std::string>                         cmd1;
    std::queue<std::pair<std::string, std::string>> cmd2;

    int firstArg = parse_options(argc, argv, &cmd1, &cmd2);

    if (OptionParser::has_flag('n', argc, argv)) {
      lt_log_print(torrent::LOG_WARN, "Ignoring rtorrent.rc.");
    } else {
      char* config_dir = std::getenv("XDG_CONFIG_HOME");
      char* home_dir   = std::getenv("HOME");

      if (config_dir != nullptr && config_dir[0] == '/' &&
          access((std::string(config_dir) + "/rtorrent/rtorrent.rc").c_str(),
                 F_OK) != -1) {
        rpc::parse_command_single(rpc::make_target(),
                                  "try_import = " + std::string(config_dir) +
                                    "/rtorrent/rtorrent.rc");
      } else if (home_dir != nullptr && access((std::string(home_dir) +
                                                "/.config/rtorrent/rtorrent.rc")
                                                 .c_str(),
                                               F_OK) != -1) {
        rpc::parse_command_single(
          rpc::make_target(), "try_import = ~/.config/rtorrent/rtorrent.rc");
      } else if (home_dir != nullptr &&
                 access((std::string(home_dir) + "/.rtorrent.rc").c_str(),
                        F_OK) != -1) {
        rpc::parse_command_single(rpc::make_target(),
                                  "try_import = ~/.rtorrent.rc");
      } else {
        rpc::parse_command_single(rpc::make_target(),
                                  "try_import = /etc/rtorrent/rtorrent.rc");
      }
    }

    while (!cmd1.empty()) {
      const auto cmd = cmd1.front();
      rpc::parse_command_single_std(cmd);
      cmd1.pop();
    }

    while (!cmd2.empty()) {
      const auto pair = cmd2.front();
      rpc::call_command_set_std_string(pair.first, pair.second);
      cmd2.pop();
    }

    // Sorting is O(n*log(n)) and very expensive
    // RPC user does not rely on rTorrent for sorted list
    // Only set view sorting and periodic resorting when not in daemon mode
    if (!rpc::call_command_value("system.daemon")) {
      rpc::parse_command_multiple(
        rpc::make_target(),
        "view.sort_new     = name,((less,((d.name))))\n"
        "view.sort_current = name,((less,((d.name))))\n"

        "schedule2 = view.main,10,10,((view.sort,main,20))\n"
        "schedule2 = view.name,10,10,((view.sort,name,20))\n");
    }

    LT_LOG(
      "seeded srandom (seed:%u) and srand48 (seed:%l)", uint_seed, long_seed);

    control->initialize();
    control->ui()->load_input_history();

    // Load session torrents and perform scheduled tasks to ensure
    // session torrents are loaded before arg torrents.
    control->dht_manager()->load_dht_cache();

    load_session_torrents();
    load_arg_torrents(argv + firstArg, argv + argc);

    // Make sure we update the display before any scheduled tasks can
    // run, so that loading of torrents doesn't look like it hangs on
    // startup.
    control->display()->adjust_layout();
    control->display()->receive_update();

    worker_thread->start_thread();

    rpc::commands.call_catch("event.system.startup_done",
                             rpc::make_target(),
                             "startup_done",
                             "System startup_done event action failed: ");

    if (!display::Canvas::isInitialized()) {
      std::cout << "rTorrent: started, "
                << control->core()->download_list()->size()
                << " torrents loaded" << std::endl;
    }

    torrent::thread_base::event_loop(torrent::main_thread());

    control->core()->download_list()->session_save();
    control->cleanup();

  } catch (torrent::internal_error& e) {
    control->cleanup_exception();

    std::cout << "Caught internal_error: " << e.what() << std::endl
              << e.backtrace();
    lt_log_print_dump(torrent::LOG_CRITICAL,
                      e.backtrace().c_str(),
                      e.backtrace().size(),
                      "Caught internal_error: '%s'.",
                      e.what());
    return -1;

  } catch (std::exception& e) {
    control->cleanup_exception();

    std::cout << "rtorrent: " << e.what() << std::endl;
    lt_log_print(torrent::LOG_CRITICAL, "Caught exception: '%s'.", e.what());
    return -1;
  }

  cleanup_commands();

  torrent::log_cleanup();

  delete control;
  delete worker_thread;

  return 0;
}

void
handle_sigbus(int signum, siginfo_t* sa, void*) {
  if (signum != SIGBUS)
    do_panic(signum);

  SignalHandler::set_default(signum);
  display::Canvas::cleanup();

  std::stringstream output;
  output << "Caught SIGBUS, dumping stack:" << std::endl;

#ifdef LT_HAVE_BACKTRACE
  void* stackPtrs[20];

  // Print the stack and exit.
  int    stackSize    = backtrace(stackPtrs, 20);
  char** stackStrings = backtrace_symbols(stackPtrs, stackSize);

  for (int i = 0; i < stackSize; ++i)
    output << stackStrings[i] << std::endl;

#else
  output << "Stack dump not enabled." << std::endl;
#endif
  output << std::endl
         << "Error: "
         << torrent::utils::error_number(static_cast<std::errc>(sa->si_errno))
              .message()
         << std::endl;

  const char* signal_reason;

  switch (sa->si_code) {
    case BUS_ADRALN:
      signal_reason = "Invalid address alignment.";
      break;
    case BUS_ADRERR:
      signal_reason = "Non-existent physical address.";
      break;
    case BUS_OBJERR:
      signal_reason = "Object specific hardware error.";
      break;
    default:
      if (sa->si_code <= 0)
        signal_reason = "User-generated signal.";
      else
        signal_reason = "Unknown.";

      break;
  };

  output << "Signal code '" << sa->si_code << "': " << signal_reason
         << std::endl;
  output << "Fault address: " << sa->si_addr << std::endl;

  std::cout << output.rdbuf();

  if (lt_log_is_valid(torrent::LOG_CRITICAL)) {
    std::string dump = output.str();
    lt_log_print_dump(torrent::LOG_CRITICAL,
                      dump.c_str(),
                      dump.size(),
                      "Caught signal: '%s'.",
                      signal_reason);
  }

  torrent::log_cleanup();
  std::abort();
}

void
do_panic(int signum) {
  // Use the default signal handler in the future to avoid infinit
  // loops.
  SignalHandler::set_default(signum);
  display::Canvas::cleanup();

  std::stringstream output;

  output << "Caught " << SignalHandler::as_string(signum)
         << ", dumping stack:" << std::endl;

#ifdef LT_HAVE_BACKTRACE
  void* stackPtrs[20];

  // Print the stack and exit.
  int    stackSize    = backtrace(stackPtrs, 20);
  char** stackStrings = backtrace_symbols(stackPtrs, stackSize);

  for (int i = 0; i < stackSize; ++i)
    output << stackStrings[i] << std::endl;

#else
  output << "Stack dump not enabled." << std::endl;
#endif

  if (signum == SIGBUS)
    output << "A bus error probably means you ran out of diskspace."
           << std::endl;

  std::cout << output.rdbuf();

  if (lt_log_is_valid(torrent::LOG_CRITICAL)) {
    std::string dump = output.str();
    lt_log_print_dump(torrent::LOG_CRITICAL,
                      dump.c_str(),
                      dump.size(),
                      "Caught signal: '%s.",
                      strsignal(signum));
  }

  torrent::log_cleanup();
  std::abort();
}

void
print_help() {
  std::cout << "Rakshasa's BitTorrent client version " RT_VERSION "."
            << std::endl;
  std::cout << std::endl;
  std::cout
    << "All value pairs (f.ex rate and queue size) will be in the UP/DOWN"
    << std::endl;
  std::cout
    << "order. Use the up/down/left/right arrow keys to move between screens."
    << std::endl;
  std::cout << std::endl;
  std::cout << "Usage: rtorrent [OPTIONS]... [FILE]... [URL]..." << std::endl;
  std::cout << "  -D                Enable deprecated commands" << std::endl;
  std::cout << "  -h                Display this very helpful text"
            << std::endl;
  std::cout << "  -n                Don't try to load rtorrent.rc on startup"
            << std::endl;
  std::cout << "  -b <a.b.c.d>      Bind the listening socket to this IP"
            << std::endl;
  std::cout << "  -i <a.b.c.d>      Change the IP that is sent to the tracker"
            << std::endl;
  std::cout << "  -p <int>-<int>    Set port range for incoming connections"
            << std::endl;
  std::cout << "  -d <directory>    Save torrents to this directory by default"
            << std::endl;
  std::cout << "  -s <directory>    Set the session directory" << std::endl;
  std::cout << "  -o key=opt,...    Set options, see 'rtorrent.rc' file"
            << std::endl;
  std::cout << std::endl;
  std::cout << "Main view keys:" << std::endl;
  std::cout << "  backspace         Add a torrent url or path" << std::endl;
  std::cout << "  ^s                Start torrent" << std::endl;
  std::cout << "  ^d                Stop torrent or delete a stopped torrent"
            << std::endl;
  std::cout << "  ^r                Manually initiate hash checking"
            << std::endl;
  std::cout << "  ^o                Change the destination directory of the "
               "download. The torrent must be closed."
            << std::endl;
  std::cout << "  ^q                Initiate shutdown or skip shutdown process"
            << std::endl;
  std::cout << "  a,s,d,z,x,c       Adjust upload throttle" << std::endl;
  std::cout << "  A,S,D,Z,X,C       Adjust download throttle" << std::endl;
  std::cout
    << "  I                 Toggle whether torrent ignores ratio settings"
    << std::endl;
  std::cout << "  right             View torrent" << std::endl;
  std::cout << std::endl;
  std::cout << "Download view keys:" << std::endl;
  std::cout << "  spacebar          Depends on the current view" << std::endl;
  std::cout << "  1,2               Adjust max uploads" << std::endl;
  std::cout << "  3,4,5,6           Adjust min/max connected peers"
            << std::endl;
  std::cout << "  t/T               Query tracker for more peers / Force query"
            << std::endl;
  std::cout << "  *                 Snub peer" << std::endl;
  std::cout << "  right             View files" << std::endl;
  std::cout << "  p                 View peer information" << std::endl;
  std::cout << "  o                 View trackers" << std::endl;
  std::cout << std::endl;

  std::cout << "Report bugs to <github.com/Elegant996/rtorrent-jesec>." << std::endl;

  exit(0);
}
